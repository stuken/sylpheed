// sylpheed - ReXGlue Recompiled Project
//
// Draw hooks for the native renderer. The draw entry points are replaced (the
// guest bodies only build PM4 for the discard ring), while BeginVertices,
// EndVertices and DrawVerticesUP wrap the guest. XDK 2135's DrawVerticesUP
// calls BeginVertices but closes the packet inline rather than through
// EndVertices, so both ends record the pending immediate-mode draw. Vertex and index buffers are still guest-owned, so each draw
// binds host mirrors of whatever the device has set (gpu/guest_buffers.h).
// The recording sequence follows reblue's gpu/hooks/draw.cpp (Copyright (c)
// 2026 Tom Clay, BSD 3-Clause).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "core/settings.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/guest_buffers.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_INT32(sylpheed_trace_frame, 0, kCvarGroup,
                     "Log every draw and resolve of present frame N (0 = off, "
                     "-N = N frames into the first 3D scene)");
double SylpheedMissionSeconds(); // platform/input_script.cpp

REXCVAR_DEFINE_DOUBLE(sylpheed_trace_at, 0.0, kCvarGroup,
                      "Log every draw and resolve of the first frame this many "
                      "seconds into the mission (0 = off)");

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_EndVertices);
REX_EXTERN(__imp__D3DDevice_DrawVerticesUP);
REX_EXTERN(__imp__D3DDevice_SetTexture);
REX_EXTERN(__imp__D3D__StreamCallback);
REX_EXTERN(__imp__D3D__StreamPackets);

namespace sylpheed::gpu {
u32 PresentCount();
u32 BoundRenderTargetVa();
}

namespace {

namespace xe = rex::graphics::xenos;
using sylpheed::gpu::D3DDevice;
using sylpheed::gpu::GuestBuffer;
using sylpheed::gpu::ResourceType;
using sylpheed::gpu::Video;

using sylpheed::gpu::D3DIndexBuffer;
using sylpheed::gpu::D3DVertexBuffer;

// Draw counters, reported by LogDrawStats from the Present hook.
struct DrawStats {
  std::atomic<u32> issued{0};
  std::atomic<u32> recorded{0};
  std::atomic<u32> no_framebuffer{0};
  std::atomic<u32> no_state{0};
  std::atomic<u32> no_index_buffer{0};
  std::atomic<u32> textures_resolved{0};
  std::atomic<u32> textures_unresolved{0};
  std::atomic<u32> resolves{0};
  std::atomic<u32> stream_callbacks{0};
  std::atomic<u32> stream_draws{0};
  std::mutex rt_mutex;
  std::unordered_map<u64, u32> draws_per_rt; // (w << 32 | h) -> draws
};
DrawStats g_stats;

struct DrawArgs {
  bool indexed = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  // Points are one guest pixel: no point sprite sizes, and DispatchDraw covers
  // the host block at a resolution scale. Lines are one host pixel wide,
  // unscaled; SYLPHEED draws none.
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    SYLPHEED_WARN_FIRST(1, "MapPrimitiveType: line list drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    SYLPHEED_WARN_FIRST(1, "MapPrimitiveType: line strip drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    // Expanded through Video::FanExpansionIBView.
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kRectangleList:
    // Expanded by ExpandRectangleList.
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
      SYLPHEED_WARN("MapPrimitiveType: unmapped primitive type {} drawn as a "
               "triangle list",
               prim);
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

// Per pixel sampler, the last texture SetTexture bound that was not null:
// what fetch constant s holds. A null SetTexture only clears device.textures[s]
// and leaves the fetch constant (and its dirty bit) alone, so the GPU keeps
// sampling the earlier texture. Sylpheed's effect system relies on that: the
// frame binds the scene colour, velocity and depth resolves and its effect
// then sets those slots to null before the composite draw (0x822CDD2C).
std::atomic<u32> g_slot_texture[16] = {};

// Whether the texture header at va still describes fetch constant fc: its
// format, base address and size words (the sampler fields differ, the device
// folds its sampler state into them). The device holds the base as a physical
// address (0x0E040000) where the header has its 0xA0000000+ mapping, so the
// top three bits are not compared.
bool HeaderMatchesFetch(u32 va, const sylpheed::gpu::DeviceFetchConstant &fc) {
  const auto *words = sylpheed::mem::try_at<const be_u32>(
      va + offsetof(sylpheed::gpu::D3DTexture, Format));
  constexpr u32 kPhysical = 0x1FFFFFFFu;
  return words && (u32(words[1]) & kPhysical) == (u32(fc.dword[1]) & kPhysical) &&
         u32(words[2]) == u32(fc.dword[2]);
}

// Pixel samplers 0..15: device.textures[s] pairs with fetch constant s.
void BindGuestTextures(const D3DDevice &dev) {
  for (u32 i = 0; i < 16; ++i) {
    u32 tex_va = dev.textures[i];
    if (!tex_va) {
      // The GPU samples the fetch constant SetTexture copied into the device,
      // not the header, and a header can be freed without D3DResource_Destroy
      // (or its memory reused) while the constant lives on. A remembered
      // header that no longer matches the constant names something else now.
      tex_va = g_slot_texture[i].load(std::memory_order_relaxed);
      if (tex_va && !HeaderMatchesFetch(tex_va, dev.fetchConstants[i])) {
        SYLPHEED_WARN_FIRST(8, "slot {}: texture 0x{:08X} no longer matches its "
                               "fetch constant (dword1 0x{:08X} dword2 0x{:08X}), "
                               "forgotten",
                            i, tex_va, u32(dev.fetchConstants[i].dword[1]),
                            u32(dev.fetchConstants[i].dword[2]));
        g_slot_texture[i].compare_exchange_strong(tex_va, 0u,
                                                  std::memory_order_relaxed);
        tex_va = 0;
      }
    }
    if (tex_va) {
      auto *tex = sylpheed::gpu::ResolveOrMirrorGuestTexture(tex_va);
      (tex ? g_stats.textures_resolved : g_stats.textures_unresolved)
          .fetch_add(1, std::memory_order_relaxed);
      Video::SetTexture(i, tex);
    }
  }
}

void CountRenderTarget() {
  auto &s = sylpheed::gpu::state();
  std::lock_guard rt_lock(g_stats.rt_mutex);
  const u64 key = s.render_target ? (u64(s.render_target->width) << 32 |
                                     s.render_target->height)
                                  : 0;
  ++g_stats.draws_per_rt[key];
}

// The guest return addresses above a hook: lr (if non-zero) first, then the
// back chain. Each frame's first word links to the caller's, and prologues
// save LR at -8 from the caller's stack pointer.
std::string GuestCallChain(u32 sp, u32 lr, u32 depth) {
  std::string chain;
  if (lr)
    chain += std::format(" {:08X}", lr);
  for (u32 i = 0; sp && i < depth; ++i) {
    // The outermost frame links past the top of the stack: checked reads.
    const auto *link = sylpheed::mem::try_at<const be_u32>(sp);
    if (!link || !u32(*link))
      break;
    sp = u32(*link);
    if (const auto *saved = sylpheed::mem::try_at<const be_u32>(sp - 8))
      chain += std::format(" {:08X}", u32(*saved));
  }
  return chain;
}

// The frame sylpheed_trace_frame=-N or sylpheed_trace_at picked, once due.
std::atomic<u32> g_auto_trace_frame{0};

// The present frame sylpheed_trace_frame names, 0 for none (yet).
u32 TraceFrame() {
  const i32 wanted = REXCVAR_GET(sylpheed_trace_frame);
  return wanted > 0 ? static_cast<u32>(wanted)
                    : g_auto_trace_frame.load(std::memory_order_relaxed);
}

// sylpheed_trace_frame=N logs every draw of present frame N.
bool TracingThisFrame() {
  const u32 frame = TraceFrame();
  return frame && sylpheed::gpu::PresentCount() + 1 == frame;
}

void TraceDraw(const char *path, const D3DDevice &dev, u32 prim, u32 count) {
  auto &s = sylpheed::gpu::state();
  auto hash = [](const sylpheed::gpu::GuestShader *gs) -> u64 {
    return gs && gs->shaderCacheEntry ? gs->shaderCacheEntry->hash : 0;
  };
  std::string tex;
  for (u32 i = 0; i < 16; ++i) {
    // A '*' marks a slot SetTexture nulled, sampled from its fetch constant.
    u32 va = dev.textures[i];
    const char *held = "";
    if (!va && (va = g_slot_texture[i].load(std::memory_order_relaxed)))
      held = "*";
    if (!va)
      continue;
    const char *kind = sylpheed::gpu::FindResolveTarget(va) ? "resolve" : "mirror";
    const auto &fc = dev.fetchConstants[i].dword;
    tex += std::format(" t{}{}=0x{:08X}({} fmt={} {}x{} swz={:03X} sign={:02X} exp={} dim={})",
                       i, held, va, kind, u32(fc[1]) & 0x3F,
                       (u32(fc[2]) & 0x1FFF) + 1,
                       ((u32(fc[2]) >> 13) & 0x1FFF) + 1,
                       (u32(fc[3]) >> 1) & 0xFFF, (u32(fc[0]) >> 2) & 0xFF,
                       static_cast<i32>(u32(fc[3]) << 13) >> 26,
                       (u32(fc[5]) >> 9) & 3);
  }
  // The game code that issued the draw.
  if (auto *ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
    const PPCContext &ctx = *ts->context();
    tex += " chain:" + GuestCallChain(ctx.r1.u32, u32(ctx.lr), 6);
  }
  SYLPHEED_INFO("[trace] {} prim={} n={} rt={}x{} vs={:016X} ps={:016X} "
           "blend={}:{:08X} depth={:08X} cull={:X} color={:X} aref={:.3f} "
           "poly={:X}:{:.3f},{:.6f} vtx={:X} vte={:X} sc={:08X}:{:08X} ucp={:X}:{:.3f},{:.3f},{:.3f},{:.3f} vp={},{} {}x{} vso={:.1f},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f}{}",
           path, prim, count, s.render_target ? s.render_target->width : 0,
           s.render_target ? s.render_target->height : 0,
           hash(s.vertex_shader), hash(s.pixel_shader),
           (u32(dev.blendFlags) >> 31), u32(dev.rbBlendControl0),
           u32(dev.rbDepthControl), u32(dev.paSuScModeCntl) & 7u,
           u32(dev.rbColorControl), float(dev.rbAlphaRef),
           (u32(dev.paSuScModeCntl) >> 11) & 7u, float(dev.polyOffsetFrontScale),
           float(dev.polyOffsetFrontOffset), u32(dev.paSuVtxCntl), u32(dev.paClVteCntl),
           u32(dev.paScWindowScissorTL), u32(dev.paScWindowScissorBR),
           u32(dev.paClClipCntl) & 0x3Fu, float(dev.clipPlanes[0][0]),
           float(dev.clipPlanes[0][1]), float(dev.clipPlanes[0][2]),
           float(dev.clipPlanes[0][3]),
           u32(dev.viewport.X), u32(dev.viewport.Y), u32(dev.viewport.Width),
           u32(dev.viewport.Height), float(dev.viewportScaleOffset[0]),
           float(dev.viewportScaleOffset[1]), float(dev.viewportScaleOffset[2]),
           float(dev.viewportScaleOffset[3]), float(dev.viewportScaleOffset[4]),
           float(dev.viewportScaleOffset[5]), tex);
}

// The PS constants of a full-screen pass, where post grading and filter taps
// live.
void TracePixelConstants(const D3DDevice &dev) {
  std::string consts;
  for (u32 c : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 32u,
                33u, 34u}) {
    const auto &v = dev.psFloatConstants[c];
    consts += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", c, float(v[0]),
                          float(v[1]), float(v[2]), float(v[3]));
  }
  SYLPHEED_INFO("[trace]   ps consts{}", consts);
}

// Binds host mirrors of the device's vertex streams and index buffer.
void BindGuestGeometry(const D3DDevice &dev, bool indexed) {
  for (u32 i = 0; i < sylpheed::gpu::kStreamCount; ++i) {
    const u32 vb_va = dev.streamBuffers[i];
    const u32 stride = u32(dev.streamStrides[i]) * 4;
    const auto *vb = sylpheed::mem::at<const D3DVertexBuffer>(vb_va);
    if (!vb_va || !vb) {
      Video::SetVertexStream(i, {}, 0, stride);
      continue;
    }
    const u32 offset = sylpheed::gpu::StreamOffset(dev, i, *vb);
    const u32 base = u32(vb->Fetch0) & ~0x3u;
    const u32 size = u32(vb->Fetch1) & 0x03FFFFFCu;
    GuestBuffer *mirror =
        sylpheed::gpu::MirrorGuestBuffer(base, size, ResourceType::VertexBuffer, 4,
                                    plume::RenderFormat::UNKNOWN);
    if (!mirror || !mirror->hasBuffer() || offset >= size) {
      Video::SetVertexStream(i, {}, 0, stride);
      continue;
    }
    Video::SetVertexStream(i, mirror->bufferRef(offset), size - offset, stride);
  }
  BindGuestTextures(dev);
  if (!indexed)
    return;
  const auto *ib = sylpheed::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  GuestBuffer *mirror = nullptr;
  if (dev.indexBuffer && ib) {
    const bool index32 =
        (u32(ib->resource.Common) & sylpheed::gpu::kCommonIndex32) != 0;
    mirror = sylpheed::gpu::MirrorGuestBuffer(
        ib->Address, ib->Size, ResourceType::IndexBuffer, index32 ? 4 : 2,
        index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  }
  Video::SetIndices(mirror);
}

// Expands the guest's indexed quad list into a triangle list (each quad
// i0 i1 i2 i3 as i0 i1 i2, i0 i2 i3, the order QuadlistExpansionIBView uses)
// in the upload heap and draws it. Caller holds the state mutex.
bool DrawIndexedQuadList(const D3DDevice &dev, const DrawArgs &args, u32 quads) {
  const auto *ibh = sylpheed::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  if (!quads || !dev.indexBuffer || !ibh)
    return false;
  const bool index32 =
      (u32(ibh->resource.Common) & sylpheed::gpu::kCommonIndex32) != 0;
  const u32 index_size = index32 ? 4u : 2u;
  const u32 ib_base = u32(ibh->Address);
  const u32 ib_size = u32(ibh->Size);
  if (u64(args.startIndex + quads * 4) * index_size > ib_size)
    return false;
  const auto *src =
      sylpheed::mem::at<const u8>(ib_base + args.startIndex * index_size);
  if (!src)
    return false;
  std::vector<u32> tris(size_t(quads) * 6);
  auto read = [&](u32 i) -> u32 {
    return index32 ? u32(reinterpret_cast<const be_u32 *>(src)[i])
                   : u32(u16(reinterpret_cast<const be_u16 *>(src)[i]));
  };
  for (u32 q = 0; q < quads; ++q) {
    const u32 i0 = read(q * 4), i1 = read(q * 4 + 1), i2 = read(q * 4 + 2),
              i3 = read(q * 4 + 3);
    u32 *o = tris.data() + size_t(q) * 6;
    o[0] = i0;
    o[1] = i1;
    o[2] = i2;
    o[3] = i0;
    o[4] = i2;
    o[5] = i3;
  }
  const u32 bytes = static_cast<u32>(tris.size() * sizeof(u32));
  auto alloc = sylpheed::gpu::UploadHostBytes(tris.data(), bytes, 4);
  if (!alloc.memory)
    return false;
  auto &s = sylpheed::gpu::state();
  const plume::RenderIndexBufferView view(alloc.ref, bytes,
                                          plume::RenderFormat::R32_UINT);
  s.command_list->setIndexBuffer(&view);
  s.command_list->drawIndexedInstanced(quads * 6, 1, 0, args.baseVertexIndex,
                                       0);
  return true;
}

// Xenos rectangle lists (primitive 8): three vertices per rectangle, and the
// GPU completes the fourth corner as a parallelogram, opposite the right-angle
// corner. The host has no such primitive, so before the draw each bound stream
// is rewritten with four vertices per rectangle (the fourth from the other
// three, per element) and bound in place of the guest buffer. Returns the
// triangle-list indices, two triangles per rectangle; empty when the draw
// cannot be expanded. stream_data(s, stride) gives the guest bytes of stream s
// from the draw's first vertex, or nullptr.
using StreamData = std::function<const u8 *(u32 stream, u32 &stride)>;

std::vector<u32> ExpandRectangleList(u32 vertex_count,
                                     const StreamData &stream_data) {
  using sylpheed::gpu::D3DDeclType;
  using sylpheed::gpu::D3DDeclUsage;
  const auto *decl = sylpheed::gpu::state().vertex_declaration;
  const u32 rects = vertex_count / 3;
  if (!decl || !rects)
    return {};
  auto be_float = [](const u8 *p) {
    const u32 v = u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
    return std::bit_cast<float>(v);
  };
  auto put_be_float = [](u8 *p, float f) {
    const u32 v = std::bit_cast<u32>(f);
    p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
  };

  // Each rectangle's right-angle corner (0..2): the vertex opposite the
  // longest edge, from the 2D position. Without a float position, vertex 0.
  std::vector<u8> corner(rects, 0);
  for (u32 e = 0; e < decl->vertexElementCount; ++e) {
    const auto &el = decl->vertexElements[e];
    if (el.usage != u8(D3DDeclUsage::kPosition) || el.usageIndex != 0)
      continue;
    const auto type = D3DDeclType(u32(el.type));
    if (type != D3DDeclType::kFloat2 && type != D3DDeclType::kFloat3 &&
        type != D3DDeclType::kFloat4)
      break;
    u32 stride = 0;
    const u8 *src = stream_data(u16(el.stream), stride);
    if (!src)
      break;
    for (u32 r = 0; r < rects; ++r) {
      float x[3], y[3];
      for (u32 i = 0; i < 3; ++i) {
        const u8 *v = src + (r * 3 + i) * stride + u16(el.offset);
        x[i] = be_float(v);
        y[i] = be_float(v + 4);
      }
      auto len2 = [&](u32 a, u32 b) {
        return (x[a] - x[b]) * (x[a] - x[b]) + (y[a] - y[b]) * (y[a] - y[b]);
      };
      const float d12 = len2(1, 2), d02 = len2(0, 2), d01 = len2(0, 1);
      corner[r] = d12 >= d02 && d12 >= d01 ? 0 : (d02 >= d01 ? 1 : 2);
    }
    break;
  }

  for (u32 s = 0; s < sylpheed::gpu::kStreamCount; ++s) {
    if (!decl->vertexStreams[s])
      continue;
    u32 stride = 0;
    const u8 *src = stream_data(s, stride);
    if (!src)
      return {};
    std::vector<u8> out(size_t(rects) * 4 * stride);
    for (u32 r = 0; r < rects; ++r) {
      const u8 *v[3] = {src + (r * 3) * stride, src + (r * 3 + 1) * stride,
                        src + (r * 3 + 2) * stride};
      u8 *dst = out.data() + size_t(r) * 4 * stride;
      std::memcpy(dst, v[0], size_t(stride) * 3);
      u8 *d = dst + 3 * stride;
      const u32 c = corner[r], a = (c + 1) % 3, b = (c + 2) % 3;
      std::memcpy(d, v[c], stride); // unknown elements keep the corner's value
      for (u32 e = 0; e < decl->vertexElementCount; ++e) {
        const auto &el = decl->vertexElements[e];
        if (u16(el.stream) != s)
          continue;
        const u32 off = u16(el.offset);
        u32 floats = 0;
        switch (D3DDeclType(u32(el.type))) {
        case D3DDeclType::kFloat1: floats = 1; break;
        case D3DDeclType::kFloat2: floats = 2; break;
        case D3DDeclType::kFloat3: floats = 3; break;
        case D3DDeclType::kFloat4: floats = 4; break;
        case D3DDeclType::kD3DColor:
        case D3DDeclType::kUByte4:
        case D3DDeclType::kUByte4Alt:
        case D3DDeclType::kUByte4N:
        case D3DDeclType::kUByte4NAlt:
          for (u32 i = 0; i < 4 && off + i < stride; ++i)
            d[off + i] = u8(std::clamp(int(v[a][off + i]) + int(v[b][off + i]) -
                                           int(v[c][off + i]),
                                       0, 255));
          continue;
        default:
          continue;
        }
        for (u32 i = 0; i < floats && off + i * 4 + 4 <= stride; ++i)
          put_be_float(d + off + i * 4,
                       be_float(v[a] + off + i * 4) + be_float(v[b] + off + i * 4) -
                           be_float(v[c] + off + i * 4));
      }
    }
    // Host vertex buffers hold the guest words byte-swapped, as uploaded.
    for (size_t i = 0; i + 4 <= out.size(); i += 4)
      std::swap(out[i], out[i + 3]), std::swap(out[i + 1], out[i + 2]);
    auto alloc = sylpheed::gpu::UploadHostBytes(out.data(), u32(out.size()), 4);
    if (!alloc.memory)
      return {};
    Video::SetVertexStream(s, alloc.ref, alloc.size, stride);
  }

  // The given triangle as is, and its mirror over the diagonal: the corner
  // swapped for the new vertex, two vertices swapped to keep the winding.
  std::vector<u32> indices;
  indices.reserve(size_t(rects) * 6);
  for (u32 r = 0; r < rects; ++r) {
    const u32 v0 = r * 4, c = corner[r];
    u32 tri[3] = {v0, v0 + 1, v0 + 2};
    indices.insert(indices.end(), tri, tri + 3);
    tri[c] = v0 + 3;
    std::swap(tri[(c + 1) % 3], tri[(c + 2) % 3]);
    indices.insert(indices.end(), tri, tri + 3);
  }
  return indices;
}

// Xenos point lists (primitive 1) draw a square per point, its size in pixels
// from the vertex shader's point size export (Sylpheed's stars) or
// PA_SU_POINT_SIZE; D3D12 points are one pixel. Each bound stream is rewritten
// with four copies of each point's vertex, which the VS spreads into the
// square's corners by vertex id (POINT_SPRITE_CORNER, under the
// pointSpriteScale shared constant). Indexed lists are gathered through the
// guest's indices. Returns the triangle-list indices, two triangles per point;
// empty when the draw cannot be expanded.
std::vector<u32> ExpandPointList(const D3DDevice &dev, const DrawArgs &args) {
  const auto *decl = sylpheed::gpu::state().vertex_declaration;
  const u32 points = args.vertexOrIndexCount;
  if (!decl || !points)
    return {};

  std::vector<u32> vertex_index(points);
  if (args.indexed) {
    const auto *ibh = sylpheed::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
    if (!dev.indexBuffer || !ibh)
      return {};
    const bool index32 =
        (u32(ibh->resource.Common) & sylpheed::gpu::kCommonIndex32) != 0;
    const u32 index_size = index32 ? 4u : 2u;
    if (u64(args.startIndex + points) * index_size > u32(ibh->Size))
      return {};
    const auto *src = sylpheed::mem::at<const u8>(u32(ibh->Address) +
                                                  args.startIndex * index_size);
    if (!src)
      return {};
    for (u32 k = 0; k < points; ++k) {
      const u32 i = index32 ? u32(reinterpret_cast<const be_u32 *>(src)[k])
                            : u32(u16(reinterpret_cast<const be_u16 *>(src)[k]));
      vertex_index[k] = u32(i32(i) + args.baseVertexIndex);
    }
  } else {
    for (u32 k = 0; k < points; ++k)
      vertex_index[k] = args.startVertex + k;
  }

  for (u32 s = 0; s < sylpheed::gpu::kStreamCount; ++s) {
    if (!decl->vertexStreams[s])
      continue;
    const u32 vb_va = dev.streamBuffers[s];
    const u32 stride = u32(dev.streamStrides[s]) * 4;
    const auto *vb = sylpheed::mem::at<const D3DVertexBuffer>(vb_va);
    if (!vb_va || !vb || !stride)
      return {};
    const u32 base = u32(vb->Fetch0) & ~0x3u;
    const u32 size = u32(vb->Fetch1) & 0x03FFFFFCu;
    const u32 offset = sylpheed::gpu::StreamOffset(dev, s, *vb);
    const auto *src = sylpheed::mem::at<const u8>(base);
    if (!src)
      return {};
    std::vector<u8> out(size_t(points) * 4 * stride);
    for (u32 k = 0; k < points; ++k) {
      const u64 at = u64(offset) + u64(vertex_index[k]) * stride;
      if (at + stride > size)
        continue; // out of range: a zero vertex, as an unfetchable one reads
      for (u32 c = 0; c < 4; ++c)
        std::memcpy(out.data() + (size_t(k) * 4 + c) * stride, src + at, stride);
    }
    // Host vertex buffers hold the guest words byte-swapped, as uploaded.
    for (size_t i = 0; i + 4 <= out.size(); i += 4)
      std::swap(out[i], out[i + 3]), std::swap(out[i + 1], out[i + 2]);
    auto alloc = sylpheed::gpu::UploadHostBytes(out.data(), u32(out.size()), 4);
    if (!alloc.memory)
      return {};
    Video::SetVertexStream(s, alloc.ref, alloc.size, stride);
  }

  // Corners 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
  std::vector<u32> indices;
  indices.reserve(size_t(points) * 6);
  for (u32 k = 0; k < points; ++k) {
    const u32 v = k * 4;
    const u32 tris[6] = {v, v + 1, v + 2, v + 2, v + 1, v + 3};
    indices.insert(indices.end(), tris, tris + 6);
  }
  return indices;
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const DrawArgs &args) {
  sylpheed::gpu::perf::Scope timer(sylpheed::gpu::perf::Counter::Draw);
  SYLPHEED_CPU_ZONE("Draw");
  const auto *dev = sylpheed::mem::at<const D3DDevice>(device_guest);
  if (!dev || !args.vertexOrIndexCount)
    return;
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  // sylpheed_trace_frame=-N: the 3D frame's post passes are the first
  // rectangle lists the game draws, so trace N frames after the first one.
  if (const i32 wanted = REXCVAR_GET(sylpheed_trace_frame);
      wanted < 0 &&
      primitive_type == static_cast<u32>(xe::PrimitiveType::kRectangleList)) {
    u32 expected = 0;
    g_auto_trace_frame.compare_exchange_strong(
        expected, sylpheed::gpu::PresentCount() + 1 + static_cast<u32>(-wanted),
        std::memory_order_relaxed);
  }
  if (TracingThisFrame()) {
    TraceDraw(args.indexed ? "DrawIndexed" : "Draw", *dev, primitive_type,
              args.vertexOrIndexCount);
    // Full-screen passes: their PS constants (post grading lives there).
    if (!args.indexed &&
        (args.vertexOrIndexCount <= 6 ||
         primitive_type == static_cast<u32>(xe::PrimitiveType::kRectangleList)))
      TracePixelConstants(*dev);
    // The first few scene draws: every non-zero VS/PS float constant below
    // c64, enough to sanity-check lighting and fog parameters.
    static u32 s_dumped_frame = 0, s_dumped = 0;
    if (s_dumped_frame != sylpheed::gpu::PresentCount()) {
      s_dumped_frame = sylpheed::gpu::PresentCount();
      s_dumped = 0;
    }
    if (args.indexed && s_dumped++ < 3) {
      for (int stage = 0; stage < 2; ++stage) {
        std::string consts;
        for (u32 c = 0; c < 64; ++c) {
          const auto &v = stage ? dev->psFloatConstants[c] : dev->vsFloatConstants[c];
          const float f[4] = {float(v[0]), float(v[1]), float(v[2]), float(v[3])};
          if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
            continue;
          consts += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", c, f[0],
                                f[1], f[2], f[3]);
        }
        SYLPHEED_INFO("[trace]   {} consts{}", stage ? "ps" : "vs", consts);
      }
    }
  }
  BindGuestGeometry(*dev, args.indexed);
  std::vector<u32> rect_indices;
  if (static_cast<xe::PrimitiveType>(primitive_type) ==
          xe::PrimitiveType::kRectangleList &&
      !args.indexed) {
    rect_indices = ExpandRectangleList(
        args.vertexOrIndexCount, [&](u32 s, u32 &stride) -> const u8 * {
          const u32 vb_va = dev->streamBuffers[s];
          stride = u32(dev->streamStrides[s]) * 4;
          const auto *vb = sylpheed::mem::at<const D3DVertexBuffer>(vb_va);
          if (!vb_va || !vb || !stride)
            return nullptr;
          const u32 base = (u32(vb->Fetch0) & ~0x3u) +
                           sylpheed::gpu::StreamOffset(*dev, s, *vb) +
                           args.startVertex * stride;
          return sylpheed::mem::at<const u8>(base);
        });
    if (rect_indices.empty())
      SYLPHEED_WARN_FIRST(4, "rectangle list of {} vertices not expanded "
                             "(indexed={})", args.vertexOrIndexCount, args.indexed);
  }
  const bool point_list = static_cast<xe::PrimitiveType>(primitive_type) ==
                          xe::PrimitiveType::kPointList;
  if (point_list) {
    // Shares the expanded-draw path below with the rectangle lists.
    rect_indices = ExpandPointList(*dev, args);
    if (rect_indices.empty())
      SYLPHEED_WARN_FIRST(4, "point list of {} vertices not expanded (indexed={})",
                          args.vertexOrIndexCount, args.indexed);
  }

  // One lock across the whole recording sequence, as in reblue: the
  // per-frame command list is single-producer.
  auto &s = sylpheed::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  Video::OpenCommandListLocked();
  const bool point_sprites = point_list && !rect_indices.empty();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      point_sprites ? plume::RenderPrimitiveTopology::TRIANGLE_LIST
                    : MapPrimitiveType(primitive_type));
  // Neither rectangles nor point sprites are face-culled.
  s.draw_ignores_cull = point_sprites ||
                        static_cast<xe::PrimitiveType>(primitive_type) ==
                            xe::PrimitiveType::kRectangleList;
  s.point_sprite_draw = point_sprites;
  if (!Video::BindDrawFramebufferLocked()) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Video::FlushRenderStateLocked(device_guest)) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;

  if (!rect_indices.empty()) {
    const u32 bytes = static_cast<u32>(rect_indices.size() * sizeof(u32));
    auto alloc = sylpheed::gpu::UploadHostBytes(rect_indices.data(), bytes, 4);
    if (!alloc.memory)
      return;
    const plume::RenderIndexBufferView view(alloc.ref, bytes,
                                            plume::RenderFormat::R32_UINT);
    cmd_list->setIndexBuffer(&view);
    cmd_list->drawIndexedInstanced(static_cast<u32>(rect_indices.size()), 1, 0,
                                   0, 0);
    s.dirtyStates.indices = true;
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    u32 quads = args.vertexOrIndexCount / 4;
    quads = std::min(quads, Video::QuadlistMaxQuads());
    const auto *ib = Video::QuadlistExpansionIBView();
    if (args.indexed) {
      // An indexed quad list (the nitrous radial blur's grid) names its own
      // corners, so split the guest's quads rather than consecutive vertices.
      if (DrawIndexedQuadList(*dev, args, quads))
        s.dirtyStates.indices = true;
    } else if (quads && ib) {
      const i32 first = args.indexed ? args.baseVertexIndex
                                     : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, first, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kTriangleFan &&
             !args.indexed) {
    const u32 tris = std::min(args.vertexOrIndexCount >= 3
                                  ? args.vertexOrIndexCount - 2
                                  : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(tris * 3, 1, 0,
                                     static_cast<i32>(args.startVertex), 0);
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    if (static_cast<xe::PrimitiveType>(primitive_type) ==
        xe::PrimitiveType::kTriangleFan) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
        SYLPHEED_WARN("indexed triangle fan drawn as a triangle list");
    }
    if (s.index_view.buffer.ref == nullptr) {
      g_stats.no_index_buffer.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kPointList &&
             sylpheed::gpu::TargetScaleLocked(s) > 1) {
    // A one-pixel guest point covers scale x scale host pixels. Host points
    // are one pixel, so draw the list once per host pixel of the block, each
    // pass moved onto that pixel's centre. SYLPHEED's only point draw is the sun
    // visibility count: 64 additive points summing the probe into one pixel
    // that the glow reads back bilinearly, so every host pixel of the block
    // needs the full sum.
    const u32 scale = sylpheed::gpu::TargetScaleLocked(s);
    const float centre = 0.5f * float(scale) - 0.5f;
    for (u32 y = 0; y < scale; ++y) {
      for (u32 x = 0; x < scale; ++x) {
        sylpheed::gpu::SetHostPixelNudge(float(x) - centre, float(y) - centre);
        if (!Video::FlushRenderStateLocked(device_guest))
          break;
        cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex,
                                0);
      }
    }
    sylpheed::gpu::SetHostPixelNudge(0.0f, 0.0f);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
}

u32 D3DDevice_DrawVertices_hook(u32 device, u32 primitive_type,
                                u32 start_vertex, u32 vertex_count) {
  DrawArgs args;
  args.vertexOrIndexCount = vertex_count;
  args.startVertex = start_vertex;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// All five arguments are marshaled: IndexCount is in r7.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive_type,
                                       u32 base_vertex, u32 start_index,
                                       u32 index_count) {
  DrawArgs args;
  args.indexed = true;
  args.vertexOrIndexCount = index_count;
  args.baseVertexIndex = static_cast<i32>(base_vertex);
  args.startIndex = start_index;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// BeginVertices hands the game a pointer it fills, EndVertices draws it. The
// guest allocator returns discard command memory that stays valid until the
// next wrap, which is far longer than one Begin/End pair.
struct PendingUP {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data = 0;
};
thread_local PendingUP g_pending;

} // namespace

namespace sylpheed::gpu {

// For the other hooks' sylpheed_trace_frame lines.
bool TraceFrameActive() { return TracingThisFrame(); }

// Called by the Present hook after present `frame`: arms sylpheed_trace_at,
// and moves an auto trace on when its frame drew nothing (some presents
// repeat the last frame).
void MaybeScheduleTrace(u32 frame) {
  static bool armed = false;
  static u32 issued_before = 0;
  const u32 issued = g_stats.issued.load(std::memory_order_relaxed);
  if (frame == g_auto_trace_frame.load(std::memory_order_relaxed) &&
      issued == issued_before) {
    g_auto_trace_frame.store(frame + 1, std::memory_order_relaxed);
  }
  issued_before = issued;
  const double at = REXCVAR_GET(sylpheed_trace_at);
  if (armed || at <= 0.0 || SylpheedMissionSeconds() < at)
    return;
  armed = true;
  u32 expected = 0;
  g_auto_trace_frame.compare_exchange_strong(expected, frame + 1,
                                             std::memory_order_relaxed);
  SYLPHEED_INFO("sylpheed_trace_at: tracing from present frame {}", frame + 1);
}

// Draw totals so far, logged by the Present hook under sylpheed_devmode.
void LogDrawStats(u32 frame) {
  SYLPHEED_DEV_INFO("draws after frame {}: {} issued, {} recorded, skipped {} no "
           "framebuffer, {} state, {} no index buffer",
           frame, g_stats.issued.load(), g_stats.recorded.load(),
           g_stats.no_framebuffer.load(), g_stats.no_state.load(),
           g_stats.no_index_buffer.load());
  std::string rts;
  {
    std::lock_guard lock(g_stats.rt_mutex);
    for (const auto &[size, n] : g_stats.draws_per_rt)
      rts += std::format(" {}x{}:{}", size >> 32, size & 0xFFFFFFFFu, n);
  }
  SYLPHEED_DEV_INFO("  textures {} resolved, {} unresolved, {} resolves; {} "
               "stream callbacks drew {}; draws per RT:{}",
               g_stats.textures_resolved.load(),
               g_stats.textures_unresolved.load(), g_stats.resolves.load(),
               g_stats.stream_callbacks.load(), g_stats.stream_draws.load(), rts);
}

} // namespace sylpheed::gpu

// (device, primitive type, vertex count, stride) -> vertex pointer
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  PendingUP p{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0};
  __imp__D3DDevice_BeginVertices(ctx, base);
  p.data = ctx.r3.u32;
  if (TracingThisFrame())
    SYLPHEED_INFO("[trace] BeginVertices prim={} n={} stride={} data=0x{:08X}",
             p.primitive_type, p.vertex_count, p.stride, p.data);
  g_pending = p;
}

namespace {
// Records the draw BeginVertices left pending, once its vertices are written.
void FlushPendingUP() {
  const PendingUP p = g_pending;
  g_pending = {};
  if (!p.data || !p.vertex_count || !p.stride)
    return;
  auto alloc = sylpheed::gpu::UploadGuestBytesByteSwap32(
      p.data, p.vertex_count * p.stride, /*alignment=*/4);
  if (!alloc.memory)
    return;
  if (const auto *dev = sylpheed::mem::at<const D3DDevice>(p.device)) {
    if (TracingThisFrame()) {
      TraceDraw("DrawUP", *dev, p.primitive_type, p.vertex_count);
      if (p.vertex_count <= 6) {
        std::string verts;
        const auto *f = sylpheed::mem::at<const be_f32>(p.data);
        for (u32 i = 0; f && i < p.vertex_count * p.stride / 4; ++i)
          verts += std::format("{}{:.4f}", i % (p.stride / 4) ? "," : " | ",
                               float(f[i]));
        SYLPHEED_INFO("[trace]   verts{}", verts);
        TracePixelConstants(*dev);
      }
    }
    BindGuestTextures(*dev);
  }
  Video::SetVertexStream(0, alloc.ref, alloc.size, p.stride);
  // Streams 1..15 of the device must not leak into an immediate-mode draw.
  for (u32 i = 1; i < 16; ++i)
    Video::SetVertexStream(i, {}, 0, 0);
  const bool rect_list = static_cast<xe::PrimitiveType>(p.primitive_type) ==
                         xe::PrimitiveType::kRectangleList;
  std::vector<u32> rect_indices;
  if (rect_list) {
    // Sylpheed's sky backdrop is one immediate-mode rectangle.
    rect_indices = ExpandRectangleList(
        p.vertex_count, [&](u32 s, u32 &stride) -> const u8 * {
          stride = p.stride;
          return s == 0 ? sylpheed::mem::at<const u8>(p.data) : nullptr;
        });
  }

  auto &s = sylpheed::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  Video::OpenCommandListLocked();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(p.primitive_type));
  s.draw_ignores_cull = rect_list;
  s.point_sprite_draw = false;
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  if (!Video::BindDrawFramebufferLocked()) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Video::FlushRenderStateLocked(p.device) || !s.command_list) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
  if (!rect_indices.empty()) {
    const u32 bytes = static_cast<u32>(rect_indices.size() * sizeof(u32));
    auto ib = sylpheed::gpu::UploadHostBytes(rect_indices.data(), bytes, 4);
    if (!ib.memory)
      return;
    const plume::RenderIndexBufferView view(ib.ref, bytes,
                                            plume::RenderFormat::R32_UINT);
    s.command_list->setIndexBuffer(&view);
    s.command_list->drawIndexedInstanced(static_cast<u32>(rect_indices.size()),
                                         1, 0, 0, 0);
    s.dirtyStates.indices = true;
  } else if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    const u32 quads = std::min(p.vertex_count / 4, Video::QuadlistMaxQuads());
    if (const auto *ib = Video::QuadlistExpansionIBView(); quads && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(quads * 6, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
             xe::PrimitiveType::kTriangleFan) {
    const u32 tris = std::min(p.vertex_count >= 3 ? p.vertex_count - 2 : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(tris * 3, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else {
    s.command_list->drawInstanced(p.vertex_count, 1, 0, 0);
  }
}

} // namespace

// (device)
REX_HOOK_RAW(D3DDevice_EndVertices) {
  __imp__D3DDevice_EndVertices(ctx, base);
  FlushPendingUP();
}

// (device, primitive type, vertex count, data, stride)
REX_HOOK_RAW(D3DDevice_DrawVerticesUP) {
  g_pending = {};
  __imp__D3DDevice_DrawVerticesUP(ctx, base);
  FlushPendingUP();
}

// D3D callback streams. Between D3D__BeginCallbackStream and
// D3D__EndCallbackStream the game records entries for the GPU to run later:
// D3D__StreamCallback queues a CPU callback (fn, context) and
// D3D__StreamPackets the data block it consumes, after flushing the render
// state the game set for it. On the console the GPU interrupts the CPU at each
// callback, which then writes its draws (through the D3D__StreamWriter*
// functions) into command space the GPU set aside, and the GPU runs them in
// place. Sylpheed's debug-line overlay (0x822F6910: AI routes, bounding
// boxes; off in the retail game) records every batch that way: the callback
// copies the batch's vertex records out of its packets and draws them as one
// line list or triangle strip.
//
// With no GPU timeline, the native renderer runs each callback as soon as its
// packets are recorded, on the recording thread, while the device still holds
// the state the batch was recorded under. The writer hooks hand the callback a
// scratch buffer for its vertices and turn its draws into immediate-mode draws.
namespace {

struct PendingStreamCallback {
  u32 fn = 0;
  u32 context = 0;
};
thread_local PendingStreamCallback g_stream_callback;
// The device of the stream whose callback is running, for the writer hooks.
thread_local u32 g_stream_device = 0;

// Guest memory the running callback copies its vertices into. Each callback
// draws one batch of at most 256 records (4 KiB); the buffer is reused per
// callback.
constexpr u32 kStreamScratchSize = 256u << 10;
u32 g_stream_scratch = 0;
thread_local u32 g_stream_scratch_used = 0;

u32 StreamScratch() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_stream_scratch =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(kStreamScratchSize, 0x1000);
  });
  return g_stream_scratch;
}

} // namespace

// (device, fn, context): queued for the packets that follow.
REX_HOOK_RAW(D3D__StreamCallback) {
  g_stream_callback = {ctx.r4.u32, ctx.r5.u32};
  __imp__D3D__StreamCallback(ctx, base);
}

// (device, type, data, bytes). The guest body flushes the dirty state and
// copies the block into the stream; then the callback queued for it runs.
REX_HOOK_RAW(D3D__StreamPackets) {
  const u32 device = ctx.r3.u32;
  const u32 data = ctx.r5.u32;
  __imp__D3D__StreamPackets(ctx, base);
  const PendingStreamCallback cb = g_stream_callback;
  g_stream_callback = {};
  const u32 scratch = StreamScratch();
  if (!cb.fn || !scratch)
    return;
  // fn(stream context, context, packet data, first pass). The context the
  // GPU would pass only reaches the writer hooks, which ignore it.
  g_stats.stream_callbacks.fetch_add(1, std::memory_order_relaxed);
  g_stream_device = device;
  g_stream_scratch_used = 0;
  u64 args[] = {scratch, cb.context, data, 0};
  auto *ks = REX_KERNEL_STATE();
  ks->function_dispatcher()->ExecuteTrap(
      rex::system::XThread::GetCurrentThread()->thread_state(), cb.fn, args,
      std::size(args));
  g_stream_device = 0;
}

// (writer, stream context): the writer state lives on the callback's stack
// and only these hooks read it.
REX_HOOK_RAW(D3D__StreamWriterBegin) { (void)base; (void)ctx; }

REX_HOOK_RAW(D3D__StreamWriterEnd) { (void)base; (void)ctx; }

// (writer, bytes, alignment) -> where the callback copies its data.
REX_HOOK_RAW(D3D__StreamWriterAlloc) {
  (void)base;
  const u32 bytes = ctx.r4.u32;
  const u32 offset = (g_stream_scratch_used + 127u) & ~127u;
  if (!g_stream_scratch || offset + bytes > kStreamScratchSize) {
    SYLPHEED_WARN_FIRST(4, "callback stream: {} bytes do not fit the scratch "
                           "buffer", bytes);
    ctx.r3.u64 = g_stream_scratch; // the copy lands, the draw is dropped
    g_stream_scratch_used = kStreamScratchSize;
    return;
  }
  g_stream_scratch_used = offset + bytes;
  ctx.r3.u64 = g_stream_scratch + offset;
}

// (writer, primitive, vertex count, vertex data): a non-indexed draw fetching
// stream 0 from the data, at the stride SetStreamSource gave it.
REX_HOOK_RAW(D3D__StreamWriterDraw) {
  (void)base;
  const u32 device = g_stream_device;
  const auto *dev = sylpheed::mem::at<const D3DDevice>(device);
  if (!dev || !g_stream_scratch || g_stream_scratch_used >= kStreamScratchSize)
    return;
  g_pending = {device, ctx.r4.u32, ctx.r5.u32,
               u32(dev->streamStrides[0]) * 4, ctx.r6.u32};
  g_stats.stream_draws.fetch_add(1, std::memory_order_relaxed);
  if (TracingThisFrame())
    SYLPHEED_INFO("[trace] StreamDraw prim={} n={} stride={} data=0x{:08X}",
                  g_pending.primitive_type, g_pending.vertex_count,
                  g_pending.stride, g_pending.data);
  FlushPendingUP();
}

// (writer, &pass) -> nonzero to run the callback's body again for the next
// pass, which it starts that many records into its data. One pass here.
REX_HOOK_RAW(D3D__StreamWriterNextPass) {
  (void)base;
  ctx.r3.u64 = 0;
}

// (device, flags, src rect, dest texture, dest point, level, face, clear
// color, ClearZ in f1, stencil, params). The guest body only builds PM4.
// A resolve into a texture copies the bound render target into that texture's
// host resolve target, created at the source's format. Afterwards the guest
// clears through ClearF when asked: D3DRESOLVE_CLEARRENDERTARGET (0x100)
// clears the resolved target's color from pClearColor (four floats),
// D3DRESOLVE_CLEARDEPTHSTENCIL (0x200) depth from f1 and stencil from the
// caller's stack at r1+92 (0x82258980 reads 508(r1) under a 416-byte frame).
// Depth resolves are not modeled yet, as in reblue.
constexpr u32 kResolveClearRenderTarget = 0x100;
constexpr u32 kResolveClearDepthStencil = 0x200;

u32 FloatColorToArgb(const be_f32 *c) {
  auto byte = [](float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u32>(v * 255.0f + 0.5f);
  };
  return byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

REX_HOOK_RAW(D3DDevice_Resolve) {
  (void)base;
  const u32 flags = ctx.r4.u32;
  const u32 dest = ctx.r6.u32;
  const u32 level = ctx.r8.u32;
  const u32 face = ctx.r9.u32;
  const u32 clear_color_va = ctx.r10.u32;
  const double clear_z = ctx.f1.f64;
  const u32 *stencil_slot = sylpheed::mem::at<const u32>(ctx.r1.u32 + 92);
  const u32 clear_stencil =
      stencil_slot ? u32(*reinterpret_cast<const be_u32 *>(stencil_slot)) : 0;
  ctx.r3.u64 = 0;

  sylpheed::gpu::GuestTexture *src = sylpheed::gpu::state().render_target;
  if (TracingThisFrame()) {
    // D3DRECT {x1,y1,x2,y2}, D3DPOINT {x,y}; the surface words carry the
    // EDRAM base and pitch the guest resolves from.
    std::string rect = "full", point = "0,0", words;
    if (const auto *r = sylpheed::mem::at<const be_u32>(ctx.r5.u32); r && ctx.r5.u32)
      rect = std::format("{},{}-{},{}", u32(r[0]), u32(r[1]), u32(r[2]), u32(r[3]));
    if (const auto *p = sylpheed::mem::at<const be_u32>(ctx.r7.u32); p && ctx.r7.u32)
      point = std::format("{},{}", u32(p[0]), u32(p[1]));
    point += " chain:" + GuestCallChain(ctx.r1.u32, 0, 6);
    const u32 surface_va = sylpheed::gpu::BoundRenderTargetVa();
    if (const auto *w = sylpheed::mem::at<const be_u32>(surface_va); w && surface_va)
      for (int i = 12; i < 15; ++i)
        words += std::format(" {:08X}", u32(w[i]));
    SYLPHEED_INFO("[trace] Resolve flags=0x{:X} dest=0x{:08X} src={}x{} rect={} "
             "point={} surface=0x{:08X}:{} lr=0x{:08X}",
             flags, dest, src ? src->width : 0, src ? src->height : 0, rect,
             point, surface_va, words, u32(ctx.lr));
  }
  // A depth resolve copies the bound depth buffer into an R32_FLOAT texture,
  // which a k_24_8(_FLOAT) fetch reads depth from in .x. Sylpheed's motion
  // blur composite samples the scene's to keep the blur off nearer geometry
  // and writes it back out as oDepth, so the effects drawn over the composite
  // stay behind the ship.
  if (dest && (flags & sylpheed::gpu::kResolveDepthStencil)) {
    sylpheed::gpu::GuestTexture *depth = sylpheed::gpu::state().depth_stencil;
    if (depth && depth->texture &&
        depth->sampleCount == plume::RenderSampleCount::COUNT_1) {
      if (auto *dst = sylpheed::gpu::GetOrCreateResolveTarget(
              dest, plume::RenderFormat::R32_FLOAT)) {
        Video::TrackResolveSource(flags, dst, level, face);
        Video::ResolveRtToTexture(dst, nullptr, 0, 0);
        g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      SYLPHEED_WARN_FIRST(4, "Resolve: depth resolve into 0x{:08X} with no "
                             "single-sampled depth buffer bound", dest);
    }
  }
  // The guest clears only the resolved rect; the host clears the whole target.
  if (ctx.r5.u32 &&
      (flags & (kResolveClearRenderTarget | kResolveClearDepthStencil)))
    SYLPHEED_WARN_FIRST(4, "Resolve: clear limited to a source rect clears the "
                   "whole target (flags=0x{:X})", flags);
  if (dest && !(flags & sylpheed::gpu::kResolveDepthStencil) && src &&
      src->texture) {
    if (auto *dst = sylpheed::gpu::GetOrCreateResolveTarget(dest, src->format)) {
      Video::TrackResolveSource(flags, dst, level, face);
      // D3DRECT {x1,y1,x2,y2} in surface pixels and D3DPOINT {x,y}.
      plume::RenderRect rect;
      const plume::RenderRect *rect_ptr = nullptr;
      if (const auto *r = sylpheed::mem::at<const be_i32>(ctx.r5.u32); r && ctx.r5.u32) {
        rect = plume::RenderRect(i32(r[0]), i32(r[1]), i32(r[2]), i32(r[3]));
        rect_ptr = &rect;
      }
      u32 point_x = 0, point_y = 0;
      if (const auto *p = sylpheed::mem::at<const be_u32>(ctx.r7.u32); p && ctx.r7.u32) {
        point_x = p[0];
        point_y = p[1];
      }
      Video::ResolveRtToTexture(dst, rect_ptr, point_x, point_y);
      g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
    }
  }

  u32 clear_flags = 0;
  u32 color = 0;
  if (flags & kResolveClearRenderTarget) {
    clear_flags |= sylpheed::gpu::kClearTarget;
    if (const auto *c = sylpheed::mem::at<const be_f32>(clear_color_va))
      color = FloatColorToArgb(c);
  }
  if (flags & kResolveClearDepthStencil)
    clear_flags |= sylpheed::gpu::kClearZBuffer | sylpheed::gpu::kClearStencil;
  if (clear_flags)
    Video::Clear(clear_flags, color, static_cast<float>(clear_z), clear_stencil);
}

// (device, sampler, texture, dirty mask). Wraps the guest, which keeps
// device.textures[] and the fetch constants, and notes the last non-null
// texture per pixel sampler for BindGuestTextures.
REX_HOOK_RAW(D3DDevice_SetTexture) {
  const u32 sampler = ctx.r4.u32;
  const u32 texture = ctx.r5.u32;
  __imp__D3DDevice_SetTexture(ctx, base);
  if (texture && sampler < std::size(g_slot_texture))
    g_slot_texture[sampler].store(texture, std::memory_order_relaxed);
}

namespace sylpheed::gpu {
u32 TraceFrameNumber() { return TraceFrame(); }

// A destroyed texture leaves the slots that still name it.
void ForgetSlotTexture(u32 texture_va) {
  for (auto &slot : g_slot_texture) {
    u32 expected = texture_va;
    slot.compare_exchange_strong(expected, 0u, std::memory_order_relaxed);
  }
}
} // namespace sylpheed::gpu

REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
