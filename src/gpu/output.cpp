/**
 * @file    gpu/output.cpp
 * @brief   Output geometry: the render size, the aspect the frame is built
 *          for, and the fit that centers one inside the other.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/output.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/ui/window.h>

#include "core/logging.h"
#include "core/settings.h"
#include "gpu/device.h"
#include "gpu/settings.h"

REXCVAR_DEFINE_INT32(sylpheed_resolution_scale, 1, kCvarGroup,
                     "Integer render resolution scale over the guest's 1280x720 "
                     "(0 = from the output height, 1 = native)");

namespace sylpheed::gpu {

namespace {

std::atomic<u64> g_render_size{0};
std::atomic<u32> g_generation{0};

bool ConfiguredResolution(u32 &w, u32 &h) {
  i32 cfg_w = 0;
  i32 cfg_h = 0;
  if (!rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(cfg_w,
                                                                      cfg_h))
    return false;
  w = std::clamp<u32>(static_cast<u32>(cfg_w), 320u, 16384u);
  h = std::clamp<u32>(static_cast<u32>(cfg_h), 240u, 16384u);
  return true;
}

u64 FitInside(u32 sw, u32 sh) {
  i32 off_x = 0;
  i32 off_y = 0;
  u32 fit_w = 0;
  u32 fit_h = 0;
  // sylpheed does not reproject, so the picture keeps the guest's 16:9 and is
  // pillarboxed or letterboxed inside the window.
  Output::ComputeFit(sw, sh, kDesignCanvasAspect, fit_w, fit_h, off_x, off_y);
  return (fit_w && fit_h) ? ((u64(fit_w) << 32) | fit_h) : 0;
}

} // namespace

void Output::Init(rex::ui::Window *window) {
  u32 sw = 0;
  u32 sh = 0;
  if (!ConfiguredResolution(sw, sh)) {
    sw = window->GetActualPhysicalWidth();
    sh = window->GetActualPhysicalHeight();
  }
  g_render_size.store(FitInside(sw, sh), std::memory_order_relaxed);
}

bool Output::Recompute() {
  const u64 current = g_render_size.load(std::memory_order_relaxed);
  if (!current)
    return false;

  u32 sw = 0;
  u32 sh = 0;
  if (!ConfiguredResolution(sw, sh)) {
    sw = Video::OutputWidth();
    sh = Video::OutputHeight();
  }
  const u64 next = FitInside(sw, sh);
  if (!next || next == current)
    return false;
  g_render_size.store(next, std::memory_order_relaxed);
  g_generation.fetch_add(1, std::memory_order_release);
  SYLPHEED_INFO("[output-res] render rect -> {}x{}", u32(next >> 32), u32(next));
  return true;
}

u32 Output::Generation() {
  return g_generation.load(std::memory_order_acquire);
}

bool Output::RenderSize(u32 &w, u32 &h) {
  const u64 packed = g_render_size.load(std::memory_order_relaxed);
  if (!packed)
    return false;
  w = static_cast<u32>(packed >> 32);
  h = static_cast<u32>(packed);
  return true;
}

u32 Output::ResolutionScale() {
  static const u32 scale = [] {
    // 1440 * 8 stays inside D3D12's 16384 texture limit for the race target.
    constexpr u32 kMaxScale = 8;
    constexpr u32 kMaxBaseScale = 4;
    const i32 wanted = REXCVAR_GET(sylpheed_resolution_scale);
    u32 base = 1;
    u32 w = 0, h = 0;
    const bool sized = RenderSize(w, h) && h;
    if (wanted > 0)
      base = std::min(u32(wanted), kMaxBaseScale);
    else if (sized)
      base = std::clamp((h + u32(kDesignCanvasHeight) - 1) /
                            u32(kDesignCanvasHeight),
                        1u, kMaxBaseScale);
    // sylpheed_supersampling 2 renders at twice that and Present downsamples.
    const u32 ss = Settings::Get().SuperSampling() == 2 ? 2u : 1u;
    const u32 out = std::min(base * ss, kMaxScale);
    SYLPHEED_INFO("resolution scale {}x ({}x{} render, sylpheed_resolution_scale={}, "
             "sylpheed_supersampling={})",
             out, w, h, wanted, ss);
    return out;
  }();
  return scale;
}

u32 Output::SurfaceScale(u32 guest_width, u32 guest_height) {
  constexpr u32 kGlowWidth = u32(kDesignCanvasWidth) / 4;
  constexpr u32 kGlowHeight = u32(kDesignCanvasHeight) / 4;
  if (guest_width == kGlowWidth && guest_height == kGlowHeight)
    return 1;
  return ResolutionScale();
}

double Output::RenderAspect() {
  u32 w = 0, h = 0;
  if (RenderSize(w, h) && h)
    return static_cast<double>(w) / static_cast<double>(h);
  return 16.0 / 9.0;
}

void Output::ComputeFit(u32 swapW, u32 swapH, double aspect, u32 &fitW,
                        u32 &fitH, i32 &offX, i32 &offY) {
  u32 w = swapW;
  u32 h = swapH;
  if (swapW && swapH && aspect > 0.0) {
    const double ar = aspect;
    if (static_cast<double>(swapW) >= static_cast<double>(swapH) * ar) {
      // Wider than the target ratio, so limit by height and pillarbox.
      h = swapH;
      w = static_cast<u32>(static_cast<double>(swapH) * ar + 0.5);
    } else {
      // Narrower, so limit by width and letterbox.
      w = swapW;
      h = static_cast<u32>(static_cast<double>(swapW) / ar + 0.5);
    }
  }
  w = (w & ~7u);
  h = (h & ~7u);
  if (w > swapW)
    w = swapW & ~7u;
  if (h > swapH)
    h = swapH & ~7u;
  fitW = w;
  fitH = h;
  offX = static_cast<i32>((swapW - w) / 2);
  offY = static_cast<i32>((swapH - h) / 2);
}

} // namespace sylpheed::gpu
