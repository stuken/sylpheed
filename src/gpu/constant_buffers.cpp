/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/output.h"

REXCVAR_DECLARE(bool, sylpheed_scaled_post_filters);

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif


namespace sylpheed::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, and the fetch constants almost never change between draws, so
// a 24-byte compare replaces them on the hot path. Sampler heap slots are never
// reclaimed, so a cached index stays valid until device teardown.
struct SamplerSlotCache {
  u8 fc[sizeof(DeviceFetchConstant)]{};
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[16];
  float shadowPcfScale = 1.0f;
  float nudgeX = 0.0f; // SetHostPixelNudge, in host pixels
  float nudgeY = 0.0f;
  bool sharedBound = false;
  bool ready = false;
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

// g_ShadowPcfScale is part of the shared-constant layout XenosRecomp emits but
// no SYLPHEED shader reads it (the recompiler's shadow kernel rewrite is off under
// RR6_RECOMP), so it stays at its identity.
void RecomputeShadowPcfScale(UploadState &s) { s.shadowPcfScale = 1.0f; }

bool CreateChunk(UploadChunk &chunk) {
  auto *device = sylpheed::gpu::Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = sylpheed::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                                plume::RenderBufferFlag::VERTEX |
                                plume::RenderBufferFlag::INDEX |
                                plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    SYLPHEED_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    SYLPHEED_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    SYLPHEED_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      SYLPHEED_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so a guest's
// degenerate constants (e.g. a 0/0 weight normalization when an effect's
// intensity is zero) are harmless on hardware. The recompiled host shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant would propagate through a
// composite. No SYLPHEED shader reads a float constant register as an integer
// (asuint only appears in the shared vertex fetch helpers), so the flush cannot
// corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = sylpheed::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    if constexpr (kFlushNaN) {
      const __m128i is_nan =
          _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
      v = _mm_andnot_si128(is_nan, v);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), v);
  }
#endif
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() { upload_state().sharedBound = false; }

// SYLPHEED renders the race at 1440x800 and resolves it into the 1280x720 frame
// with four bilinear taps (ps_82060208) in a rotated-grid pattern:
// sampling_point01/23 (c0, c1) are UV offsets at 1/16, 3/16, 5/16 and 7/16 of
// an output pixel. At a resolution scale the output pixel shrinks but the
// offsets don't, so the taps spread over several host pixels and soften the
// frame. Scaling the pattern about its centroid fits it to one host pixel
// again. The centroid itself stays: it offsets the quarter-pixel shift the
// guest gives the full-screen quad.
constexpr u64 kRaceResolvePSHash = 0x799300B9E6D4210Bull;
void FitResolvePatternToHostPixel(u8 *block) {
  if (!REXCVAR_GET(sylpheed_scaled_post_filters))
    return;
  auto &vs = sylpheed::gpu::state();
  auto *ps = vs.pipelineState.pixelShader;
  if (!ps || !ps->shaderCacheEntry ||
      ps->shaderCacheEntry->hash != kRaceResolvePSHash)
    return;
  const u32 scale = sylpheed::gpu::TargetScaleLocked(vs);
  if (scale <= 1)
    return;
  // c0.xy, c0.zw, c1.xy, c1.zw: the four tap offsets.
  auto *taps = reinterpret_cast<float *>(block);
  for (u32 axis = 0; axis < 2; ++axis) {
    float centroid = 0.0f;
    for (u32 k = 0; k < 4; ++k)
      centroid += taps[2 * k + axis];
    centroid *= 0.25f;
    for (u32 k = 0; k < 4; ++k) {
      float &offset = taps[2 * k + axis];
      offset = centroid + (offset - centroid) / float(scale);
    }
  }
}

ConstantAllocation UploadVertexShaderConstants(u32 device_guest) {
  SYLPHEED_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         kConstantBlockBytes);
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest) {
  SYLPHEED_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         kConstantBlockBytes);
  FitResolvePatternToHostPixel(alloc.memory);
  return alloc;
}

void SetHostPixelNudge(float x, float y) {
  auto &s = upload_state();
  s.nudgeX = x;
  s.nudgeY = y;
}

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  SYLPHEED_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // sylpheed_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: draws bind every device texture slot through
  // Video::SetTexture, which mirrors Xenos semantics (a null bind is ignored,
  // since on hardware it does not rebuild the fetch constant), so vs.textures
  // holds the last real texture per slot, exactly what the GPU would still be
  // sampling.
  auto &vs = sylpheed::gpu::state();
  const auto *device_p = sylpheed::mem::at<const D3DDevice>(device_guest);
  for (u32 i = 0; i < 16; ++i) {
    s.shared.samplerIndices[i] = 0;
    s.shared.texture2DIndices[i] = sylpheed::gpu::kNullTexture2DDescriptorIndex;
    s.shared.texture3DIndices[i] = sylpheed::gpu::kNullTexture3DDescriptorIndex;
    s.shared.textureCubeIndices[i] = sylpheed::gpu::kNullTextureCubeDescriptorIndex;

    sylpheed::gpu::GuestTexture *tex =
        vs.sample_overrides[i] ? vs.sample_overrides[i]
                           : sylpheed::gpu::SampledTextureLocked(vs, vs.textures[i]);
    if (tex && tex->descriptorIndex != sylpheed::gpu::kInvalidDescriptorIndex) {
      // The components the fetch degammas ride in the index's top bits
      // (shader_common.h TEXTURE_INDEX, textureGamma).
      const u32 gamma = u32(tex->gammaMask) << 28;
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        s.shared.texture3DIndices[i] = tex->descriptorIndex | gamma;
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                    sylpheed::gpu::kInvalidDescriptorIndex) {
          s.shared.texture2DIndices[i] = tex->companion2D->descriptorIndex | gamma;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        s.shared.textureCubeIndices[i] = tex->descriptorIndex | gamma;
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        s.shared.texture2DIndices[i] = tex->descriptorIndex | gamma;
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
            std::memcmp(sc.fc, &fc_be, sizeof(sc.fc)) == 0) {
          s.shared.samplerIndices[i] = sc.sampler;
        } else {
          const u32 fc[6] = {
              u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
              u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
          };
          auto desc = DecodeFromFetch(fc);

          // Shell fur volumes encode shell depth in W, and X360-default WRAP
          // wraps a z=0 fetch's second tap to the tip slice and halves density,
          // so force CLAMP to keep both taps on the dense base slice.
          if (clamp3d) {
            desc.addressW = plume::RenderTextureAddressMode::CLAMP;
          }
          const u32 resolved = ResolveSlotLocked(desc);
          std::memcpy(sc.fc, &fc_be, sizeof(sc.fc));
          sc.sampler = resolved;
          sc.aniso = aniso_now;
          sc.clamp3d = clamp3d;
          sc.valid = true;
          s.shared.samplerIndices[i] = resolved;
        }
      }
    }
  }

  // Shader bool constants, straight from the device's shadow (b0..b255 in the
  // layout BOOL_BIT reads). Sylpheed's effects branch on them: EX2's sky only
  // samples its map under g_MapEnable (b128) and otherwise writes a flat
  // colour over the whole frame.
  for (u32 i = 0; i < 8; ++i)
    s.shared.booleansArr[i] = device_p ? u32(device_p->boolConstants[i]) : 0u;

  s.shared.alphaThreshold = sylpheed::gpu::Video::AlphaThreshold();
  s.shared.alphaTestFunc = sylpheed::gpu::Video::AlphaTestFunc();
  // PA_SU_VTX_CNTL.PIX_CENTER 0 (D3D9 pixel centres, SYLPHEED's default) shifts
  // every vertex half a pixel right and down, as xenia does, so geometry the
  // guest pre-offset by -0.5 (the 2x Pac-Man quad) lands on host pixel edges
  // instead of sampling exactly at texel boundaries.
  const auto *dev = sylpheed::mem::at<const D3DDevice>(device_guest);
  const bool d3d9_pixel_center = dev && (u32(dev->paSuVtxCntl) & 0x1u) == 0;
  s.shared.halfPixelOffsetX = d3d9_pixel_center && vs.viewport.width > 0.0f
                                  ? 1.0f / vs.viewport.width
                                  : 0.0f;
  s.shared.halfPixelOffsetY = d3d9_pixel_center && vs.viewport.height > 0.0f
                                  ? -1.0f / vs.viewport.height
                                  : 0.0f;
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;

  // D3DRS_VIEWPORTENABLE off (PA_CL_VTE_CNTL x/y scale and offset clear):
  // the VS outputs render target pixels, as the sun visibility probe quads
  // do, so map them onto the host viewport's NDC.
  {
    const u32 vte = dev ? u32(dev->paClVteCntl) : 0x43Fu;
    const float vw = vs.viewport.width, vh = vs.viewport.height;
    const bool x_window = (vte & 0x3u) == 0 && vw > 0.0f;
    const bool y_window = (vte & 0xCu) == 0 && vh > 0.0f;
    s.shared.ndcScaleX = x_window ? 2.0f / vw : 1.0f;
    s.shared.ndcOffsetX = x_window ? -1.0f - 2.0f * vs.viewport.x / vw : 0.0f;
    s.shared.ndcScaleY = y_window ? -2.0f / vh : 1.0f;
    s.shared.ndcOffsetY = y_window ? 1.0f + 2.0f * vs.viewport.y / vh : 0.0f;
    // The viewport is in guest pixels; a host pixel is 1/scale of one.
    const float scale = float(sylpheed::gpu::TargetScaleLocked(vs));
    if (s.nudgeX != 0.0f && vw > 0.0f)
      s.shared.ndcOffsetX += 2.0f * s.nudgeX / (scale * vw);
    if (s.nudgeY != 0.0f && vh > 0.0f)
      s.shared.ndcOffsetY -= 2.0f * s.nudgeY / (scale * vh);
  }

  // At a resolution scale above 1, an edge the guest placed less than half a
  // guest pixel inside the viewport still covers the outer guest pixel on
  // hardware (coverage is tested at guest pixel centres), but would miss the
  // host sub-pixels nearest the edge: SYLPHEED's full-screen stretch sits a quarter
  // pixel in and left the 3x frame's left and top host pixels at the clear
  // colour. The VS snaps such vertices onto the edge.
  {
    const bool scaled = sylpheed::gpu::TargetScaleLocked(vs) > 1;
    s.shared.edgeSnapX = scaled && vs.viewport.width > 0.0f
                             ? 1.0f / vs.viewport.width
                             : 0.0f;
    s.shared.edgeSnapY = scaled && vs.viewport.height > 0.0f
                             ? 1.0f / vs.viewport.height
                             : 0.0f;
  }

  // Point lists are drawn as four copies of each vertex, spread by the VS into
  // a square of the point's size (Sylpheed's stars export theirs). A size in
  // pixels covers size / viewport of NDC on each side of the centre.
  s.shared.pointSpriteScaleX =
      vs.point_sprite_draw && vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.pointSpriteScaleY =
      vs.point_sprite_draw && vs.viewport.height > 0.0f ? 1.0f / vs.viewport.height : 0.0f;

  // SYLPHEED's env map pass splits each half of its target along the diagonal
  // with clip plane 0, drawing the scene once per side.
  s.shared.clipPlaneEnable = dev ? u32(dev->paClClipCntl) & 0x3Fu : 0u;
  for (u32 i = 0; i < 6; ++i)
    for (u32 j = 0; j < 4; ++j)
      s.shared.clipPlanes[i][j] =
          (s.shared.clipPlaneEnable >> i) & 1u ? float(dev->clipPlanes[i][j]) : 0.0f;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  return alloc;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  SYLPHEED_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = sylpheed::mem::at<const u8>(guest_va);
  if (!src) {
    SYLPHEED_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

} // namespace sylpheed::gpu
