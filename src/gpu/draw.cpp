/**
 * @file    gpu/draw.cpp
 * @brief   What every draw flushes: the guest render state read, the PSO
 *          lookup, and the constant buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <cmath>
#include <cstddef>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_recorder.h"

namespace sylpheed::gpu {
namespace {

// Render state comes straight from the Xenos register shadows SYLPHEED's
// render-state setters write (src/gpu/d3d.h, config/d3d.toml). Each setter
// inserts its raw D3D value into the register field, so the fields decode
// with the same D3DCMP / D3DSTENCILOP / D3DCULL / D3DBLEND converters reblue
// uses.
//   RB_DEPTHCONTROL: STENCIL_ENABLE[0] Z_ENABLE[1] Z_WRITE_ENABLE[2]
//     ZFUNC[6:4] BACKFACE_ENABLE[7] STENCILFUNC[10:8] STENCILFAIL[13:11]
//     STENCILZPASS[16:14] STENCILZFAIL[19:17] and the _BF copies above.
//   RB_BLENDCONTROL0: COLOR_SRCBLEND[4:0] COLOR_COMB_FCN[7:5]
//     COLOR_DESTBLEND[12:8] ALPHA_SRCBLEND[20:16] ALPHA_COMB_FCN[23:21]
//     ALPHA_DESTBLEND[28:24].
//   RB_COLORCONTROL: ALPHA_FUNC[2:0] ALPHA_TEST_ENABLE[3].
//   PA_SU_SC_MODE_CNTL: D3DCULL in [2:0], D3DFILLMODE in [10:3].
//   RB_STENCILREFMASK: REF[7:0] MASK[15:8] WRITEMASK[23:16].
void ReadDeviceRenderState(VideoState &s, u32 device_guest) {
  const auto *dev = sylpheed::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  const u32 blend = dev->rbBlendControl0;
  const u32 depth = dev->rbDepthControl;
  const u32 color = dev->rbColorControl;
  const u32 mode = dev->paSuScModeCntl;
  const u32 stencil = dev->rbStencilRefMask;

  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;

  // The setter writes ONE/ZERO into the register while blending is off, so
  // the D3D flag is the one to test rather than the register contents.
  Video::SetDirtyValue(dirty, ps.alphaBlendEnable,
                       (u32(dev->blendFlags) & 0x80000000u) != 0);
  Video::SetDirtyValue(dirty, ps.srcBlend, ConvertBlendMode(blend & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOp, ConvertBlendOp((blend >> 5) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlend,
                       ConvertBlendMode((blend >> 8) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.srcBlendAlpha,
                       ConvertBlendMode((blend >> 16) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOpAlpha,
                       ConvertBlendOp((blend >> 21) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlendAlpha,
                       ConvertBlendMode((blend >> 24) & 0x1Fu));

  Video::SetDirtyValue(dirty, ps.zEnable, (depth & 0x2u) != 0);
  Video::SetDirtyValue(dirty, ps.zWriteEnable, (depth & 0x4u) != 0);
  Video::SetDirtyValue(dirty, ps.zFunc, ConvertCompareFunc((depth >> 4) & 0x7u));
  Video::SetDirtyValue(dirty, ps.cullMode,
                       ConvertCullMode(s.draw_ignores_cull ? 0u : mode & 0x7u));
  Video::SetDirtyValue(dirty, ps.fillMode, ConvertFillMode((mode >> 3) & 0xFFu));
  Video::SetDirtyValue(dirty, ps.colorWriteEnable,
                       u32(dev->rbColorMask) & 0xFu);

  Video::SetDirtyValue(dirty, ps.stencilEnable, (depth & 0x1u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilTwoSided, (depth & 0x80u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilFunc,
                       ConvertCompareFunc((depth >> 8) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFail,
                       ConvertStencilOp((depth >> 11) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilPass,
                       ConvertStencilOp((depth >> 14) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilZFail,
                       ConvertStencilOp((depth >> 17) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilRef, static_cast<u8>(stencil & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilMask,
                       static_cast<u8>((stencil >> 8) & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilWriteMask,
                       static_cast<u8>((stencil >> 16) & 0xFFu));

  // Polygon offset, as xenia's GetPreferredFacePolygonOffset picks it: the
  // front face's unless that face is culled. Every polygon offset SYLPHEED sets is
  // on a triangle draw, so the point/line enable (bit 13) is not consulted.
  float offset_scale = 0.0f;
  float offset = 0.0f;
  if ((mode & (1u << 11)) && !(mode & 0x1u)) {
    offset_scale = float(dev->polyOffsetFrontScale);
    offset = float(dev->polyOffsetFrontOffset);
  }
  if ((mode & (1u << 12)) && !(mode & 0x2u) && !offset_scale && !offset) {
    offset_scale = float(dev->polyOffsetBackScale);
    offset = float(dev->polyOffsetBackOffset);
  }
  // The offset is in depth units (D24 ULPs on the host, as xenia's
  // GetD3D10IntegerPolygonOffset rounds it away from zero) and the scale in
  // 1/16 subpixels.
  const float bias_units = std::ceil(std::abs(offset) * 16777216.0f);
  const float depth_bias = offset < 0.0f ? -bias_units : bias_units;
  const float slope_bias = offset_scale * (1.0f / 16.0f);
  if (s.dynamic_depth_bias) {
    Video::SetDirtyValue(s.dirtyStates.depthBias, s.depth_bias, depth_bias);
    Video::SetDirtyValue(s.dirtyStates.depthBias, s.slope_scaled_depth_bias,
                         slope_bias);
  } else {
    // Baked into the PSO: the shadow quads' slope bias tracks the camera
    // every frame (11 to 270 over an attract demo lap), so round it up to the
    // next 2^(k/8), steps of 9% (more offset is the safe direction). 0.25
    // steps still made about a thousand pipelines over a few laps.
    float slope = 0.0f;
    if (const float mag = std::abs(slope_bias); mag > 0.0f)
      slope = std::copysign(std::exp2(std::ceil(std::log2(mag) * 8.0f) / 8.0f),
                            slope_bias);
    Video::SetDirtyValue(dirty, ps.depthBias, static_cast<i32>(depth_bias));
    Video::SetDirtyValue(dirty, ps.slopeScaledDepthBias, slope);
  }

  // The guest's own D3D keeps the viewport at +0x3208: its SetRenderTarget
  // resets it to the surface, and SetViewport overrides it (SYLPHEED renders the
  // race into all of a 1440x800 target whose depth surface is smaller).
  const D3DViewport9 &vp = dev->viewport;
  if (u32(vp.Width) && u32(vp.Height)) {
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.x, float(u32(vp.X)));
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.y, float(u32(vp.Y)));
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.width,
                         float(u32(vp.Width)));
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.height,
                         float(u32(vp.Height)));
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.minDepth,
                         float(vp.MinZ));
    Video::SetDirtyValue(s.dirtyStates.viewport, s.viewport.maxDepth,
                         float(vp.MaxZ));
    if (s.dirtyStates.viewport)
      s.dirtyStates.scissorRect = true;
  }

  // The window scissor is what the hardware clips to. SYLPHEED's env map pass
  // draws each half of its 256x512 target twice, split by scissor rects.
  const u32 tl = dev->paScWindowScissorTL;
  const u32 br = dev->paScWindowScissorBR;
  const plume::RenderRect scissor{static_cast<i32>(tl & 0x7FFFu),
                                  static_cast<i32>((tl >> 16) & 0x7FFFu),
                                  static_cast<i32>(br & 0x7FFFu),
                                  static_cast<i32>((br >> 16) & 0x7FFFu)};
  const bool guest_scissor =
      scissor.right > scissor.left && scissor.bottom > scissor.top;
  Video::SetDirtyValue(s.dirtyStates.scissorRect, s.guest_scissor, guest_scissor);
  Video::SetDirtyValue(s.dirtyStates.scissorRect, s.scissor.left, scissor.left);
  Video::SetDirtyValue(s.dirtyStates.scissorRect, s.scissor.top, scissor.top);
  Video::SetDirtyValue(s.dirtyStates.scissorRect, s.scissor.right, scissor.right);
  Video::SetDirtyValue(s.dirtyStates.scissorRect, s.scissor.bottom,
                       scissor.bottom);

  // Fixed-function alpha test on Xenos; the recompiled PS compares on the
  // spec constant against g_AlphaThreshold with ALPHA_FUNC instead.
  Video::SetAlphaTestMode((color & 0x8u) != 0);
  Video::SetAlphaTestFunc(color & 0x7u);
  Video::SetAlphaThreshold(static_cast<float>(dev->rbAlphaRef));
}
} // namespace

bool Video::FlushRenderState(u32 device_guest) {
  std::lock_guard lock(state().mutex);
  return FlushRenderStateLocked(device_guest);
}

bool Video::FlushRenderStateLocked(u32 device_guest) {
  auto &s = state();
  // A confirmed device-removed event is terminal: stop recording so the render
  // thread cannot race the fatal dialog into a crash.
  if (DeviceIsLost())
    return false;
  if (!s.command_list_open)
    return false;
  if (!s.draw_framebuffer_bound)
    return false;
  // CPU zone: a GPU zone here would add two GPU timestamps per draw.
  SYLPHEED_CPU_ZONE("FlushRenderState");

  // Fold the Set*-hook mirrors into the pipelineState the PSO lookup reads.
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexShader,
                s.vertex_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.pixelShader,
                s.pixel_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexDeclaration,
                s.vertex_declaration);

  for (u32 i = 0; i < 16; ++i) {
    SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexStrides[i],
                  static_cast<u8>(s.input_slots[i].stride));
  }

  // The PSO's formats must match the framebuffer BindDrawFramebuffer attached,
  // so they come from the same ResolveEffectiveTargets pair it binds.
  {
    GuestTexture *rt = nullptr;
    GuestTexture *ds = nullptr;
    ResolveEffectiveTargets(s, rt, ds);
    const auto rt_format = rt ? rt->format : plume::RenderFormat::UNKNOWN;
    const auto ds_format = ds ? ds->format : plume::RenderFormat::UNKNOWN;
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.renderTargetFormat, rt_format);
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.depthStencilFormat, ds_format);
  }

  // RB_DEPTHCONTROL + RB_BLENDCONTROL0 + RB_STENCILREFMASK + the color control
  // alpha enable bit, from the register shadows SYLPHEED's render-state setters
  // write.
  ReadDeviceRenderState(s, device_guest);
  // BindDrawFramebufferLocked has already flushed; pick up a guest viewport.
  FlushViewport();

  // Anything missing here means the engine has not wired the pipeline up yet.
  if (!s.pipelineState.vertexShader || !s.pipelineState.vertexDeclaration) {
    u32 n;
    if (DiagShouldLog(3, s.render_target, &n)) {
      SYLPHEED_DEV_WARN("[draw-diag] #{} draw dropped: vs={} decl={} ps={} rt={}x{}", n,
             static_cast<void *>(s.pipelineState.vertexShader),
             static_cast<void *>(s.pipelineState.vertexDeclaration),
             static_cast<void *>(s.pipelineState.pixelShader),
             s.render_target ? s.render_target->width : 0,
             s.render_target ? s.render_target->height : 0);
    }
    return false;
  }

  // D3D12 retains the bound pipeline across draws in a command list, so a clean
  // pipelineState can skip both the cache lookup and the bind.
  // BeginCommandList force-dirties this on every command list reset.
  if (s.dirtyStates.pipelineState) {
    PipelineState lookup = s.pipelineState;
    SanitizePipelineState(lookup);
    bool built = false;
    auto *pso = GetOrCreatePipeline(lookup, &built);
    if (!pso) {
      u32 n;
      if (DiagShouldLog(4, s.render_target, &n)) {
        SYLPHEED_DEV_WARN("[draw-diag] #{} draw dropped: PSO build failed (vs={} ps={} "
               "rt={}x{} fmt={})",
               n, static_cast<void *>(s.pipelineState.vertexShader),
               static_cast<void *>(s.pipelineState.pixelShader),
               s.render_target ? s.render_target->width : 0,
               s.render_target ? s.render_target->height : 0,
               u32(s.pipelineState.renderTargetFormat));
      }
      return false;
    }
    // 'built' means this draw compiled the PSO synchronously because the
    // precache list lacks it (or its background compile had not finished).
    RecordPipelineState(lookup, CurrentRenderPassId(), built);
    s.command_list->setPipeline(pso);
    s.current_pso = pso;
    // Vulkan drops dynamic state when a pipeline without it (the resolve
    // copies) was bound in between, so every guest bind sets it again.
    s.dirtyStates.depthBias = true;
  } else if (!s.current_pso) {
    // Clean dirty bits but no PSO bound: the first draw after a command list
    // reset that lost the force-dirty.
    return false;
  }

  // reblue dirty-tracks these through Set*ShaderConstant hooks. SYLPHEED writes
  // its constants through unhooked guest code (and the UI's per-element alpha
  // lives in them), so there is no dirty signal and they upload every draw,
  // like SharedConstants below.
  // Vulkan push offsets 0/8/16 follow the guest PushConstants member order
  // emitted by the recompiler.
  s.dirtyStates.vertexShaderConstants = true;
  s.dirtyStates.pixelShaderConstants = true;
  if (device_guest) {
    if (s.dirtyStates.vertexShaderConstants) {
      auto vs_alloc = UploadVertexShaderConstants(device_guest);
      if (vs_alloc.size) {
#if defined(SYLPHEED_D3D12)
        s.command_list->setGraphicsRootDescriptor(vs_alloc.ref, 0);
#else
        s.command_list->setGraphicsPushConstants(
            kGuestPushConstantRangeIndex, &vs_alloc.gpuAddress, 0, sizeof(u64));
#endif
      }
    }

    if (s.dirtyStates.pixelShaderConstants) {
      auto ps_alloc = UploadPixelShaderConstants(device_guest);
      if (ps_alloc.size) {
#if defined(SYLPHEED_D3D12)
        s.command_list->setGraphicsRootDescriptor(ps_alloc.ref, 1);
#else
        s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                                 &ps_alloc.gpuAddress,
                                                 sizeof(u64), sizeof(u64));
#endif
      }
    }

    // SharedConstants rebuilds from live guest state every draw: the sampler
    // fetch constants are written by unhooked recompiled code, so there is no
    // dirty signal. The upload is skipped internally when the built block is
    // byte-identical to the one already bound on this list.
    auto sc_alloc = UploadSharedConstants(device_guest);
    if (sc_alloc.size) {
#if defined(SYLPHEED_D3D12)
      s.command_list->setGraphicsRootDescriptor(sc_alloc.ref, 2);
#else
      s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                               &sc_alloc.gpuAddress,
                                               2 * sizeof(u64), sizeof(u64));
#endif
    }
  }

  // Lens flare occlusion count: the counter UAV (root descriptor 3 on D3D12,
  // set 4 on Vulkan) for the sun test quad draw bracketed by D3DQuery_Issue
  // BEGIN/END. The pipeline cache swaps occlusion_count_ps in while counting.
  if (s.occlusion_counting) {
    const u32 slot = s.frame.load(std::memory_order_relaxed);
    if (s.occlusion_counter[slot]) {
#if defined(SYLPHEED_D3D12)
      s.command_list->setGraphicsRootDescriptor(
          s.occlusion_counter[slot]->at(0), 3);
#else
      s.command_list->setGraphicsDescriptorSet(
          s.occlusion_descriptor_set[slot].get(), kOcclusionDescriptorSetIndex);
#endif
    }
  }

  if (s.dynamic_depth_bias && s.dirtyStates.depthBias) {
    s.command_list->setDepthBias(s.depth_bias, 0.0f, s.slope_scaled_depth_bias);
  }

  // Clean state is first=255, last=0, so 'first <= last' skips the call when
  // nothing changed. BeginCommandList force-dirties the full range every
  // command list reset: D3D12 IA bindings do not survive begin().
  if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
    const u32 first = s.dirtyStates.vertexStreamFirst;
    const u32 count = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
    s.command_list->setVertexBuffers(first, s.vertex_views + first, count,
                                     s.input_slots + first);
  }

  // Re-binds only when SetIndices changed buffer/size/format, or
  // BeginCommandList force-dirtied after a command list reset.
  if (s.dirtyStates.indices && s.index_view.buffer.ref != nullptr) {
    s.command_list->setIndexBuffer(&s.index_view);
  }

  s.dirtyStates = DirtyStates(false);
  return true;
}

} // namespace sylpheed::gpu
