/**
 * @file    gpu/output.h
 * @brief   The render rectangle (the guest's 16:9 fit inside the window), the
 *          guest's 1280x720 canvas, and the resolution scale.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace rex::ui {
class Window;
}

namespace sylpheed::gpu {

// Every HUD coordinate, debug text glyph position and 2D quad in the game is
// authored against these dims. Output res changes what the renderer targets,
// never this.
constexpr float kDesignCanvasWidth = 1280.0f;
constexpr float kDesignCanvasHeight = 720.0f;
constexpr double kDesignCanvasAspect = kDesignCanvasWidth / kDesignCanvasHeight;

class Output {
public:
  static void Init(rex::ui::Window *window);

  static bool Recompute();
  static u32 Generation();

  static bool RenderSize(u32 &w, u32 &h);

  // sylpheed: the integer factor every render surface, resolve target, viewport and
  // scissor is scaled by, so the guest's 1280x720 renders at the output's
  // resolution. sylpheed_resolution_scale, or with 0 (auto) the render height over
  // 720 rounded up, times 2 under sylpheed_supersampling 2. Latched on first use:
  // surfaces already created keep it.
  static u32 ResolutionScale();

  // The scale of one guest surface or resolve target: ResolutionScale(), but
  // 1 for SYLPHEED's 320x180 glow buffer. Its 8-tap blurs step by game constants
  // sized for 320 texels and rely on each bilinear tap spanning a whole
  // texel, so a scaled buffer leaves gaps between the taps and the glow
  // breaks into bands. Every 320x180 target SYLPHEED binds is that glow chain.
  static u32 SurfaceScale(u32 guest_width, u32 guest_height);

  // The render rect's own ratio: 16:9, give or take the multiple-of-8
  // rounding of the fit.
  static double RenderAspect();

  // fitW/fitH round down to a multiple of 8. An aspect of 0 fills the
  // rectangle, leaving no bars.
  static void ComputeFit(u32 swapW, u32 swapH, double aspect, u32 &fitW,
                         u32 &fitH, i32 &offX, i32 &offY);
};

} // namespace sylpheed::gpu
