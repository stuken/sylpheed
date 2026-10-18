// sylpheed - ReXGlue Recompiled Project
//
// Render target, depth and vertex declaration hooks. Like the shader hooks
// these wrap the guest originals: SYLPHEED's D3D keeps its own surfaces and
// declarations, the host keeps a mirror keyed by the guest object, and the
// binding state follows reblue's gpu/hooks/state.cpp (Copyright (c) 2026 Tom
// Clay, BSD 3-Clause).

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/shaders/shader_constants.h"
#include "gpu/surface_registry.h"
#include "gpu/vertex_declaration.h"

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_SetSurfaces);
REX_EXTERN(__imp__D3DDevice_SetFVF);
REX_EXTERN(__imp__D3DDevice_CreateVertexDeclaration);
REX_EXTERN(__imp__D3DDevice_SetVertexDeclaration);
REX_EXTERN(__imp__D3DSurface_GetDesc);

namespace sylpheed::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
bool ForgetGuestSurface(u32 surface_va);
bool TraceFrameActive();
}

namespace {

using sylpheed::gpu::GuestTexture;
using sylpheed::gpu::GuestVertexDeclaration;

std::mutex g_mutex;
std::unordered_map<u32, GuestTexture *> g_surfaces; // guest surface -> host
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;
u32 g_default_back_buffer = 0;
std::atomic<u32> g_bound_render_target{0};

using SurfaceDesc = sylpheed::gpu::D3DSurfaceDesc;

// Runs the guest's own GetDesc on an isolated frame, with the descriptor on
// the guest stack below the caller's live frames.
bool GuestSurfaceDesc(PPCContext &ctx, u8 *base, u32 surface,
                      SurfaceDesc &out) {
  rex::CallFrame frame(ctx);
  const u32 desc_va = (ctx.r1.u32 - 0x100) & ~0xFu;
  frame.ctx.r1.u64 = desc_va - 0x80;
  frame.ctx.r3.u64 = surface;
  frame.ctx.r4.u64 = desc_va;
  __imp__D3DSurface_GetDesc(frame.ctx, base);
  const auto *desc = sylpheed::mem::at<const SurfaceDesc>(desc_va);
  if (!desc)
    return false;
  out = *desc;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    SYLPHEED_INFO("D3DSurface_GetDesc(0x{:08X}): format=0x{:08X} type={} {}x{} "
             "msaa={}",
             surface, u32(out.Format), u32(out.Type), u32(out.Width),
             u32(out.Height), u32(out.MultiSampleType));
  }
  return out.Width != 0 && out.Height != 0;
}

GuestTexture *HostSurface(PPCContext &ctx, u8 *base, u32 surface) {
  if (!surface)
    return nullptr;
  if (surface == g_default_back_buffer)
    return sylpheed::gpu::state().back_buffer_surface;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_surfaces.find(surface); it != g_surfaces.end())
      return it->second;
  }
  SurfaceDesc desc{};
  if (!GuestSurfaceDesc(ctx, base, surface, desc))
    return nullptr;
  // The host surface is the guest size times its scale.
  const u32 scale =
      sylpheed::gpu::Output::SurfaceScale(u32(desc.Width), u32(desc.Height));
  GuestTexture *host = sylpheed::gpu::SurfaceRegistry::Get(
      u32(desc.Width) * scale, u32(desc.Height) * scale, desc.Format, 1);
  if (host)
    host->hostScale = scale;
  std::lock_guard lock(g_mutex);
  g_surfaces[surface] = host;
  return host;
}

} // namespace

namespace sylpheed::gpu {

// The device hook creates the default back buffer through the guest and
// names it here, so binding it targets the host back buffer Present shows.
void NoteDefaultSurfaces(u32 back_buffer, u32 /*depth*/) {
  g_default_back_buffer = back_buffer;
}

u32 BoundRenderTargetVa() {
  return g_bound_render_target.load(std::memory_order_relaxed);
}

// D3DResource_Destroy on a surface. The host surface goes back to the
// registry, which reissues a free one to the next surface of the same size and
// format and retires it after it idles, so a surface created later at the
// same address is mapped afresh.
bool ForgetGuestSurface(u32 surface_va) {
  GuestTexture *host = nullptr;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_surfaces.find(surface_va);
    if (it == g_surfaces.end())
      return false;
    host = it->second;
    g_surfaces.erase(it);
  }
  if (host && host != state().back_buffer_surface)
    host->held = false;
  return true;
}

} // namespace sylpheed::gpu

namespace {
// Xenos depth shares the colour surface's EDRAM pitch, so a depth surface
// smaller than its colour target still covers it. D3D12 clips rendering to the smallest
// attachment, so the host depth texture grows to cover whatever colour target
// it is bound with (once; it keeps the larger size). Called outside the state
// lock, which ResizeTexture takes.
void GrowDepthToCoverTarget() {
  auto &s = sylpheed::gpu::state();
  GuestTexture *rt = s.render_target;
  GuestTexture *ds = s.depth_stencil;
  if (!rt || !ds || !ds->texture || rt->sampleCount != ds->sampleCount)
    return;
  if (ds->width >= rt->width && ds->height >= rt->height)
    return;
  const u32 w = std::max(ds->width, rt->width);
  const u32 h = std::max(ds->height, rt->height);
  SYLPHEED_INFO("depth surface grows {}x{} -> {}x{} to cover its render target",
           ds->width, ds->height, w, h);
  sylpheed::gpu::Video::ResizeTexture(ds, w, h);
  s.draw_framebuffer_bound = false;
}
} // namespace

namespace {
// Points the host at the guest's RT0, once the guest has bound it.
void BindRenderTarget0(PPCContext &ctx, u8 *base, u32 device_va,
                       u32 surface_va) {
  g_bound_render_target.store(surface_va, std::memory_order_relaxed);
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (sylpheed::gpu::TraceFrameActive()) {
    SurfaceDesc desc{};
    if (surface_va)
      GuestSurfaceDesc(ctx, base, surface_va, desc);
    SYLPHEED_INFO("[trace] SetRenderTarget 0x{:08X} -> {}x{} guest fmt=0x{:08X} "
             "host fmt={} lr=0x{:08X}",
             surface_va, surface ? surface->width : 0,
             surface ? surface->height : 0, u32(desc.Format),
             surface ? static_cast<int>(surface->format) : -1, u32(ctx.lr));
  }
  auto &s = sylpheed::gpu::state();
  if (s.render_target != surface)
    s.draw_framebuffer_bound = false;
  sylpheed::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.render_target, surface);
  sylpheed::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  sylpheed::gpu::Video::SetDirtyValue<plume::RenderSampleCounts>(
      s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
      surface ? surface->sampleCount : plume::RenderSampleCount::COUNT_1);
  sylpheed::gpu::Video::SetDefaultViewport(
      sylpheed::mem::at<sylpheed::gpu::D3DDevice>(device_va), surface);
  GrowDepthToCoverTarget();
}

// Points the host at the guest's depth surface, once the guest has bound it.
void BindDepthStencil(PPCContext &ctx, u8 *base, u32 device_va,
                      u32 surface_va) {
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (surface && !sylpheed::gpu::IsDepthFormat(surface->format))
    surface = nullptr;
  auto &s = sylpheed::gpu::state();
  if (s.depth_stencil != surface)
    s.draw_framebuffer_bound = false;
  sylpheed::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.depth_stencil, surface);
  sylpheed::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.depthStencilFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  if (surface)
    s.scene_depth = surface;
  // No viewport reset: on Xenos D3D binding a depth surface leaves the
  // viewport the render target set.
  (void)device_va;
  GrowDepthToCoverTarget();
}
} // namespace

// (device, index, surface). Game code reaches it only through D3D itself
// (D3DDevice_Present, D3DDevice_Initialize); it binds through SetSurfaces.
REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  const u32 device_va = ctx.r3.u32;
  const u32 index = ctx.r4.u32;
  const u32 surface_va = ctx.r5.u32;
  __imp__D3DDevice_SetRenderTarget(ctx, base);
  // MRT is not modeled, as in reblue.
  if (index != 0) {
    if (surface_va)
      SYLPHEED_WARN_FIRST(4, "SetRenderTarget: MRT slot {} (0x{:08X}) not modeled",
                     index, surface_va);
    return;
  }
  BindRenderTarget0(ctx, base, device_va, surface_va);
}

// (device, surface)
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  const u32 device_va = ctx.r3.u32;
  const u32 surface_va = ctx.r4.u32;
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  BindDepthStencil(ctx, base, device_va, surface_va);
}

// (device, {depth, RT0, RT1, RT2, RT3}, flags): binds all five slots at once,
// and is how Sylpheed's renderer (0x822CD4E8) switches targets. The guest
// leaves the result in the device; RT1..3 are not modeled.
REX_HOOK_RAW(D3DDevice_SetSurfaces) {
  const u32 device_va = ctx.r3.u32;
  __imp__D3DDevice_SetSurfaces(ctx, base);
  const auto *dev = sylpheed::mem::at<const sylpheed::gpu::D3DDevice>(device_va);
  if (!dev)
    return;
  for (u32 i = 1; i < 4; ++i)
    if (const u32 rt = dev->renderTargets[i])
      SYLPHEED_WARN_FIRST(4, "SetSurfaces: MRT slot {} (0x{:08X}) not modeled",
                          i, rt);
  BindRenderTarget0(ctx, base, device_va, dev->renderTargets[0]);
  BindDepthStencil(ctx, base, device_va, dev->depthStencil);
}

// (pVertexElements) -> D3DVertexDeclaration*
REX_HOOK_RAW(D3DDevice_CreateVertexDeclaration) {
  const u32 elements_va = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexDeclaration(ctx, base);
  const u32 decl_va = ctx.r3.u32;
  auto *elements = sylpheed::mem::at<sylpheed::gpu::GuestVertexElement>(elements_va);
  if (!decl_va || !elements)
    return;
  if (auto *decl = sylpheed::gpu::CreateVertexDeclaration(elements)) {
    std::lock_guard lock(g_mutex);
    g_decls[decl_va] = decl;
  }
}

namespace {
using sylpheed::gpu::D3DDeclType;
using sylpheed::gpu::D3DDeclUsage;

// The D3DVERTEXELEMENT9 list an FVF code stands for, all on stream 0 in FVF
// order, terminated as CreateVertexDeclaration expects. Empty if the code uses
// a part the host does not expand (blend weights).
std::vector<sylpheed::gpu::GuestVertexElement> ElementsForFvf(u32 fvf) {
  std::vector<sylpheed::gpu::GuestVertexElement> out;
  u32 offset = 0;
  auto add = [&](D3DDeclType type, D3DDeclUsage usage, u8 index, u32 size) {
    sylpheed::gpu::GuestVertexElement e{};
    e.stream = 0;
    e.offset = static_cast<u16>(offset);
    e.type = static_cast<u32>(type);
    e.usage = static_cast<u8>(usage);
    e.usageIndex = index;
    out.push_back(e);
    offset += size;
  };
  switch (fvf & 0x400Eu) { // D3DFVF_POSITION_MASK
  case 0x002: // XYZ
    add(D3DDeclType::kFloat3, D3DDeclUsage::kPosition, 0, 12);
    break;
  case 0x004: // XYZRHW
    add(D3DDeclType::kFloat4, D3DDeclUsage::kPositionT, 0, 16);
    break;
  case 0x4002: // XYZW
    add(D3DDeclType::kFloat4, D3DDeclUsage::kPosition, 0, 16);
    break;
  case 0:
    break;
  default: // XYZB1..5
    return {};
  }
  if (fvf & 0x010)
    add(D3DDeclType::kFloat3, D3DDeclUsage::kNormal, 0, 12);
  if (fvf & 0x020)
    add(D3DDeclType::kFloat1, D3DDeclUsage::kPSize, 0, 4);
  if (fvf & 0x040)
    add(D3DDeclType::kD3DColor, D3DDeclUsage::kColor, 0, 4);
  if (fvf & 0x080)
    add(D3DDeclType::kD3DColor, D3DDeclUsage::kColor, 1, 4);
  const u32 tex_count = std::min((fvf >> 8) & 0xFu, 8u);
  for (u32 i = 0; i < tex_count; ++i) {
    // D3DFVF_TEXCOORDSIZEn: 0 = 2D, 1 = 3D, 2 = 4D, 3 = 1D.
    switch ((fvf >> (16 + 2 * i)) & 3u) {
    case 0:
      add(D3DDeclType::kFloat2, D3DDeclUsage::kTexCoord, u8(i), 8);
      break;
    case 1:
      add(D3DDeclType::kFloat3, D3DDeclUsage::kTexCoord, u8(i), 12);
      break;
    case 2:
      add(D3DDeclType::kFloat4, D3DDeclUsage::kTexCoord, u8(i), 16);
      break;
    default:
      add(D3DDeclType::kFloat1, D3DDeclUsage::kTexCoord, u8(i), 4);
      break;
    }
  }
  sylpheed::gpu::GuestVertexElement end{};
  end.stream = 0xFF;
  end.type = static_cast<u32>(D3DDeclType::kUnused);
  out.push_back(end);
  return out;
}

std::unordered_map<u32, GuestVertexDeclaration *> g_fvf_decls;
} // namespace

// (device, fvf). The guest binds a declaration it builds inside the device
// (+0x2EA0) without going through SetVertexDeclaration, so the host builds the
// same layout from the FVF code itself.
REX_HOOK_RAW(D3DDevice_SetFVF) {
  const u32 fvf = ctx.r4.u32;
  __imp__D3DDevice_SetFVF(ctx, base);
  GuestVertexDeclaration *decl = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_fvf_decls.find(fvf); it != g_fvf_decls.end()) {
      decl = it->second;
    } else {
      auto elements = ElementsForFvf(fvf);
      if (elements.size() > 1)
        decl = sylpheed::gpu::CreateVertexDeclaration(elements.data());
      else
        SYLPHEED_WARN("SetFVF 0x{:X}: FVF layout not modeled", fvf);
      g_fvf_decls[fvf] = decl;
    }
  }
  sylpheed::gpu::Video::SetVertexDeclaration(decl);
}

// (device, declaration)
REX_HOOK_RAW(D3DDevice_SetVertexDeclaration) {
  const u32 decl_va = ctx.r4.u32;
  __imp__D3DDevice_SetVertexDeclaration(ctx, base);
  GuestVertexDeclaration *decl = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_decls.find(decl_va); it != g_decls.end())
      decl = it->second;
  }
  // The recompiled VS guards its R11G11B10 normal decode on this bit.
  auto &s = sylpheed::gpu::state();
  u32 spec =
      s.pipelineState.specConstants & ~sylpheed::gpu::kSpecConstantR11G11B10Normal;
  if (decl && decl->hasR11G11B10Normal)
    spec |= sylpheed::gpu::kSpecConstantR11G11B10Normal;
  sylpheed::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                      s.pipelineState.specConstants, spec);
  sylpheed::gpu::Video::SetVertexDeclaration(decl);
}

