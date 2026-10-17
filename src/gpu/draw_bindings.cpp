/**
 * @file    gpu/draw_bindings.cpp
 * @brief   The Set* binders: the guest device's texture, shader, stream and
 *          viewport state mirrored into VideoState for the draw path to read.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

#include <plume_render_interface.h>

#include "gpu/shaders/shader_constants.h"
#include "gpu/output.h"

namespace sylpheed::gpu {

// ALPHAREF mirror for SharedConstants. Set from the device's RB_ALPHA_REF
// shadow on each draw, and read when SharedConstants is rebuilt.
namespace {
std::atomic<u32> g_alpha_threshold_bits{0};
std::atomic<u32> g_alpha_test_func{6}; // GREATER_EQUAL
} // namespace

void Video::SetTexture(u32 index, GuestTexture *texture) {
  if (index >= 16)
    return;
  // A null SetTexture is not an unbind on Xenos: it clears a bookkeeping
  // pointer and leaves the fetch constant alone, so the slot keeps its last
  // non-null texture until a real bind replaces it.
  if (texture == nullptr)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (texture->texture) {
    BindTextureSRVLocked(s, texture);
    // SharedConstants substitution samples the source as a Texture2D, which an
    // MSAA descriptor cannot back.
    if (texture->sourceSurface && texture->sourceSurface->texture &&
        texture->sourceSurface->sampleCount ==
            plume::RenderSampleCount::COUNT_1) {
      BindTextureSRVLocked(s, texture->sourceSurface);
    }
  }
  s.textures[index] = texture;
}

void Video::SetVertexShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_shader = shader;
}

void Video::SetPixelShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.pixel_shader = shader;
}

void Video::SetVertexDeclaration(GuestVertexDeclaration *decl) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_declaration = decl;
}

void Video::SetIndices(GuestBuffer *indices) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.index_buffer = indices;
  plume::RenderBufferReference new_buffer{};
  u32 new_size = 0;
  plume::RenderFormat new_format = plume::RenderFormat::R16_UINT;
  if (indices && indices->hasBuffer()) {
    new_buffer = indices->bufferRef(0);
    new_size = indices->dataSize;
    // A GuestBuffer registered through a path that never read the X360
    // Common field index format bit has none pinned. D3D's default is 16-bit.
    new_format = (indices->format == plume::RenderFormat::UNKNOWN)
                     ? plume::RenderFormat::R16_UINT
                     : indices->format;
  }
  SetDirtyValue(s.dirtyStates.indices, s.index_view.buffer, new_buffer);
  SetDirtyValue(s.dirtyStates.indices, s.index_view.size, new_size);
  SetDirtyValue(s.dirtyStates.indices, s.index_view.format, new_format);
}

void Video::ForgetIndices(GuestBuffer *indices) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.index_buffer == indices)
    s.index_buffer = nullptr;
}

void Video::ScrubBufferBindings(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (u32 i = 0; i < 16; ++i) {
    if (s.vertex_views[i].buffer.ref != buffer)
      continue;
    s.vertex_views[i].buffer = plume::RenderBufferReference{};
    s.vertex_views[i].size = 0;
    const u8 b = static_cast<u8>(i);
    if (b < s.dirtyStates.vertexStreamFirst)
      s.dirtyStates.vertexStreamFirst = b;
    if (b > s.dirtyStates.vertexStreamLast)
      s.dirtyStates.vertexStreamLast = b;
  }
  if (s.index_view.buffer.ref == buffer) {
    s.index_view.buffer = plume::RenderBufferReference{};
    s.index_view.size = 0;
    s.dirtyStates.indices = true;
  }
}

void Video::SetVertexStream(u32 slot, plume::RenderBufferReference buffer,
                            u32 size, u32 stride) {
  if (slot >= 16)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  // Expanding the dirty range only on a real change lets FlushRenderState skip
  // the per-draw 16-slot setVertexBuffers, and bind just [first, last] when
  // something did move.
  bool dirty = false;
  SetDirtyValue(dirty, s.vertex_views[slot].buffer, buffer);
  SetDirtyValue(dirty, s.vertex_views[slot].size, size);
  SetDirtyValue(dirty, s.input_slots[slot].stride, stride);
  if (dirty) {
    const u8 i = static_cast<u8>(slot);
    if (i < s.dirtyStates.vertexStreamFirst)
      s.dirtyStates.vertexStreamFirst = i;
    if (i > s.dirtyStates.vertexStreamLast)
      s.dirtyStates.vertexStreamLast = i;
  }
}

void Video::MarkVSConstantsDirty() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.dirtyStates.vertexShaderConstants = true;
}

void Video::MarkPSConstantsDirty() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.dirtyStates.pixelShaderConstants = true;
}

void Video::SetAlphaThreshold(float value) {
  u32 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  g_alpha_threshold_bits.store(bits, std::memory_order_relaxed);
}

float Video::AlphaThreshold() {
  const u32 bits = g_alpha_threshold_bits.load(std::memory_order_relaxed);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void Video::SetAlphaTestFunc(u32 func) {
  g_alpha_test_func.store(func & 7u, std::memory_order_relaxed);
}

u32 Video::AlphaTestFunc() {
  return g_alpha_test_func.load(std::memory_order_relaxed);
}

void Video::SetAlphaTestMode(bool enable) {
  auto &s = state();

  u32 specConstants = s.pipelineState.specConstants & ~kSpecConstantAlphaTest;
  if (enable)
    specConstants |= kSpecConstantAlphaTest;

  SetDirtyValue<bool>(s.dirtyStates.pipelineState,
                      s.pipelineState.enableAlphaToCoverage, false);
  SetDirtyValue<u32>(s.dirtyStates.pipelineState, s.pipelineState.specConstants,
                     specConstants);
}

void Video::SetDefaultViewport(D3DDevice *device, GuestTexture *surface) {
  if (surface == nullptr)
    return;

  auto &s = state();

  // Guest units: the host surface is the guest size times its scale.
  const float scale = static_cast<float>(surface->hostScale);
  const float fwidth = static_cast<float>(surface->width) / scale;
  const float fheight = static_cast<float>(surface->height) / scale;

  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width, fwidth);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.height, fheight);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.minDepth, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.maxDepth, 1.0f);

  s.dirtyStates.scissorRect =
      s.dirtyStates.scissorRect || s.dirtyStates.viewport;

  // The guest device's viewport is left alone: the guest's D3D maintains it,
  // and draws read it back (ReadDeviceRenderState). This only covers paths
  // that draw before any guest render state flush.
  (void)device;
}

void Video::SetDesignCanvasDrain(bool on) { state().design_canvas_drain = on; }

bool Video::DesignCanvasDrain() { return state().design_canvas_drain; }

void Video::FlushViewport() {
  auto &s = state();
  if (!s.command_list)
    return;

  // s.viewport and s.scissor are in guest pixels. The host surfaces are those
  // times their scale, so both are scaled here, by the target's, and nowhere
  // else.
  const u32 scale = TargetScaleLocked(s);
  if (s.dirtyStates.viewport) {
    plume::RenderViewport vp = s.viewport;
    vp.x *= float(scale);
    vp.y *= float(scale);
    vp.width *= float(scale);
    vp.height *= float(scale);
    // MinDepth above MaxDepth stays as it is: SYLPHEED's 3D scene renders
    // reverse-Z through a MinZ=1, MaxZ=0 viewport against a GEQUAL depth test
    // cleared to 0, and both D3D12 and Vulkan map depth through an inverted
    // range. Swapping them made the farthest surface win every depth test, so
    // the ship's own far side, the stars and distant ships showed through it.
    s.command_list->setViewports(&vp, 1);
    s.dirtyStates.viewport = false;
  }

  if (s.dirtyStates.scissorRect) {
    // The guest's window scissor once a draw has read it, else the viewport.
    plume::RenderRect rc{static_cast<i32>(s.viewport.x),
                         static_cast<i32>(s.viewport.y),
                         static_cast<i32>(s.viewport.x + s.viewport.width),
                         static_cast<i32>(s.viewport.y + s.viewport.height)};
    if (s.guest_scissor)
      rc = s.scissor;
    rc.left *= i32(scale);
    rc.top *= i32(scale);
    rc.right *= i32(scale);
    rc.bottom *= i32(scale);
    s.command_list->setScissors(&rc, 1);
    s.dirtyStates.scissorRect = false;
  }
}

} // namespace sylpheed::gpu
