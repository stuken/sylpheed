/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/gpu_pass_timer.h"
#include "gpu/frame.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <plume_render_interface.h>
#if defined(SYLPHEED_D3D12)
#include <plume_d3d12.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include "gpu/gpu_profiling.h"

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/frame_dump.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/display_clock.h"
#include "gpu/output.h"
#include "gpu/settings.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_INT32(sylpheed_fps_limit, 60, kCvarGroup,
                     "Frame rate cap for presentation, 0 for none");
REXCVAR_DEFINE_BOOL(sylpheed_display_correction, true, kCvarGroup,
                    "Apply the console's HDTV display correction at present, "
                    "as D3D programs it; off shows the raw frame (brighter "
                    "mid-tones)");
REXCVAR_DEFINE_STRING(sylpheed_frame_pacing, "auto", kCvarGroup,
                      "With vsync on and a refresh rate a whole multiple of "
                      "the frame cap: auto checks the display for variable "
                      "refresh, display waits that many vblanks per frame, "
                      "timer paces on a timer");

namespace sylpheed::gpu {

namespace {

using Clock = std::chrono::steady_clock;

// Set while vsync alone holds the frame rate (see ApplyVsync), so PaceFrame
// only guards against presents that stop blocking.
bool g_display_locked = false;

// Bumped by every swap chain rebuild. A resize or a fullscreen toggle can
// change whether the display's variable refresh applies to the window.
u32 g_swapchain_epoch = 0;

#if defined(SYLPHEED_D3D12)
// Frame pacing with vsync on and a refresh rate that is a whole multiple of
// the frame cap (60 or 120 Hz at 60 fps).
//
// On a fixed refresh display the swap chain waits that many vblanks per
// frame: every frame stays on screen for the same number of refreshes, locked
// to the display. A sleep pacer can't do that at 120 Hz: any wake-up or GPU
// finish that straddles a vblank pushes a frame one refresh late, which
// judders.
//
// On a variable refresh display (FreeSync, G-Sync, HDMI VRR) that is wrong.
// The display refreshes when a frame arrives, so vblanks have no fixed period
// and "two vblanks" holds a frame for no fixed time. Presents then barely
// block, PaceFrame's 0.9x guard becomes the real pacer, and the swap chain
// fills until a long stall. On a 120 Hz FreeSync TV: presents 15 ms apart
// with a 40-50 ms stall twice a second, and frames on screen for 1 to 4+
// refreshes. There the timer pacer with a sync interval of 1 is exact: the
// display shows each frame for one refresh of 1/cap.
//
// Which kind of display it is shows only in how it behaves, so with
// sylpheed_frame_pacing=auto pacing starts on the timer and judges each second of
// steady presents:
// - On the timer, a fixed refresh display keeps vblanks at its refresh
//   period, while a variable refresh one follows the frames. Vblanks at the
//   refresh period lock to the display.
// - Locked, a fixed refresh display holds presents to the cap. If instead the
//   presents run faster than the cap, the 0.9x guard is pacing and the lock
//   isn't holding: back to the timer.
// Windows can compose a window at the fixed rate for a while before it hands
// it variable refresh, so the second check catches an early wrong lock.
enum class Pacing { Timer, Display };

struct PacingState {
  Pacing mode = Pacing::Timer;
  UINT interval = 1;
  u32 multiple = 0; // vblanks per frame the refresh rate allows, 0 for none
  bool judging = false; // auto with a multiple of 2 or more
  // What the mode was chosen for. Any change chooses again.
  bool vsync = false;
  i32 fps = 0;
  u32 hz = 0;
  u32 generation = ~0u;
  u32 epoch = ~0u;
  std::string requested;
  // The window being judged.
  Clock::time_point since{};    // the choice was made
  Clock::time_point window{};   // the window starts
  Clock::time_point last{};     // the previous ApplyVsync
  u32 presents = 0;
  u32 early = 0;                // presents under 0.95 of the frame period
  bool late = false;            // a gap over 1.5 frame periods
  u32 failed_locks = 0;
};
PacingState g_pacing;

// Each judgement skips the first moments after a change, while the display
// adjusts, then looks at a second.
constexpr auto kSettle = std::chrono::milliseconds(250);
constexpr auto kWindow = std::chrono::seconds(1);
// With no vblank clock (no adapter handle, or the desktop locked), lock to
// the display after this long, the right choice for a fixed refresh display.
constexpr auto kNoClockGiveUp = std::chrono::seconds(5);
constexpr std::size_t kMinIntervals = 30;
// A lock that fails this often stays on the timer until something changes.
constexpr u32 kMaxFailedLocks = 2;

void LogPacing(const std::string &line) {
  static std::string s_last;
  if (line == s_last)
    return;
  s_last = line;
  SYLPHEED_INFO("frame pacing: {}", line);
}

void StartWindow(PacingState &p, Clock::time_point now) {
  p.window = now + kSettle;
  p.presents = 0;
  p.early = 0;
  p.late = false;
}

void LockToDisplay(PacingState &p, Clock::time_point now,
                   const std::string &why) {
  p.mode = Pacing::Display;
  p.interval = p.multiple;
  StartWindow(p, now);
  LogPacing(std::format("{} vblank(s) per frame at {} Hz{}", p.multiple, p.hz,
                        why));
}

void PaceOnTimer(PacingState &p, Clock::time_point now,
                 const std::string &why) {
  p.mode = Pacing::Timer;
  p.interval = 1;
  StartWindow(p, now);
  LogPacing("timer" + why);
}

// The share of intervals within 10% of 'period'.
double ShareNear(const std::vector<double> &intervals, double period) {
  u32 matches = 0;
  for (double v : intervals)
    matches += std::abs(v - period) <= 0.1 * period;
  return double(matches) / double(intervals.size());
}

void Judge(PacingState &p, Clock::time_point now) {
  const double refresh_ms = 1000.0 / double(p.hz);
  const double frame_ms = 1000.0 / double(p.fps);
  if (p.mode == Pacing::Display) {
    if (p.presents && p.early * 2 > p.presents) {
      ++p.failed_locks;
      PaceOnTimer(p, now,
                  std::format(" (variable refresh display: locked to {} "
                              "vblanks, {} of {} presents came early)",
                              p.multiple, p.early, p.presents));
      return;
    }
    StartWindow(p, now);
    return;
  }
  const auto intervals = display::VBlankIntervals(p.window);
  if (intervals.size() < kMinIntervals) {
    if (now - p.since >= kNoClockGiveUp)
      LockToDisplay(p, now, " (no vblank clock to check for variable refresh)");
    else
      StartWindow(p, now);
    return;
  }
  // A loading stall says nothing about the display.
  if (p.late) {
    StartWindow(p, now);
    return;
  }
  if (ShareNear(intervals, refresh_ms) >= 0.8 &&
      p.failed_locks < kMaxFailedLocks) {
    LockToDisplay(p, now, " (fixed refresh)");
    return;
  }
  if (ShareNear(intervals, frame_ms) >= 0.8)
    LogPacing(std::format("timer (variable refresh display: vblanks follow "
                          "the {} fps cap in the {} Hz mode)",
                          p.fps, p.hz));
  StartWindow(p, now);
}
#endif

void ApplyVsync(VideoState &s) {
  if (!s.swap_chain)
    return;
  const bool vsync = Settings::Get().Vsync();
  s.swap_chain->setVsyncEnabled(vsync);
#if defined(SYLPHEED_D3D12)
  auto *chain = static_cast<plume::D3D12SwapChain *>(s.swap_chain.get());
  display::Start(chain->getWindow());
  const display::Info info = display::Current();
  const i32 fps = REXCVAR_GET(sylpheed_fps_limit);
  const std::string &requested = REXCVAR_GET(sylpheed_frame_pacing);
  const auto now = Clock::now();
  PacingState &p = g_pacing;
  if (vsync != p.vsync || fps != p.fps || info.hz != p.hz ||
      info.generation != p.generation || g_swapchain_epoch != p.epoch ||
      requested != p.requested) {
    p.vsync = vsync;
    p.fps = fps;
    p.hz = info.hz;
    p.generation = info.generation;
    p.epoch = g_swapchain_epoch;
    p.requested = requested;
    p.multiple = 0;
    p.judging = false;
    p.failed_locks = 0;
    p.since = now;
    if (vsync && fps > 0 && info.hz) {
      const u32 n = (info.hz + u32(fps) / 2) / u32(fps);
      // DXGI takes at most 4. A 1 Hz slack covers the 59.94-style rates.
      if (n >= 1 && n <= 4 && std::abs(i32(info.hz) - i32(n) * fps) <= 1)
        p.multiple = n;
    }
    if (!p.multiple || requested == "timer") {
      PaceOnTimer(p, now, "");
    } else if (p.multiple == 1 || requested == "display") {
      // At one vblank per frame both kinds of display behave alike: the
      // presents block at the refresh rate.
      LockToDisplay(p, now, "");
    } else {
      p.judging = true;
      PaceOnTimer(p, now, " (checking the display for variable refresh)");
    }
  }
  if (p.judging) {
    if (now >= p.window && p.last.time_since_epoch().count() != 0) {
      const double gap_ms =
          std::chrono::duration<double, std::milli>(now - p.last).count();
      const double frame_ms = 1000.0 / double(fps);
      ++p.presents;
      p.early += gap_ms < 0.95 * frame_ms;
      p.late |= gap_ms > 1.5 * frame_ms;
    }
    if (now - p.window >= kWindow)
      Judge(p, now);
  }
  p.last = now;
  g_display_locked = p.mode == Pacing::Display;
  chain->vsyncInterval = p.interval;
#else
  g_display_locked = false;
#endif
}

// Sleeps to 't' within a few microseconds. Windows' default sleep wakes up to
// a millisecond or more late, so wait on a high resolution timer until just
// short of it, then spin.
void SleepUntil(Clock::time_point t) {
#if defined(_WIN32)
  static thread_local HANDLE s_timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_ALL_ACCESS);
  constexpr auto kSpin = std::chrono::microseconds(700);
  const auto now = Clock::now();
  if (s_timer && t - now > kSpin) {
    LARGE_INTEGER due;
    // Relative, in 100 ns units.
    due.QuadPart = -i64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t - now - kSpin)
                            .count() /
                        100);
    if (SetWaitableTimerEx(s_timer, &due, 0, nullptr, nullptr, nullptr, 0))
      WaitForSingleObject(s_timer, INFINITE);
  }
  while (Clock::now() < t)
    YieldProcessor();
#else
  std::this_thread::sleep_until(t);
#endif
}

constexpr i32 kIdleFPS = 30;

void PaceFrame(bool idle = false) {
  static Clock::time_point next{};
  i32 fps = REXCVAR_GET(sylpheed_fps_limit);
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0) {
    next = {};
    return;
  }
  // Locked to the display, the presents already block to the frame rate, and
  // this only catches them returning early (a minimized or occluded window)
  // without ever waking past a frame's vblank.
  const double scale = g_display_locked && !idle ? 0.9 : 1.0;
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 * scale / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    SleepUntil(next);
    next += period;
  } else {
    next = now + period;
  }
}

void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

void RebuildSwapChain(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  ++g_swapchain_epoch;
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      SYLPHEED_ERROR("Swap chain resize failed");
    return;
  }
  Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  SYLPHEED_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  const double present_aspect = Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  // The guest's D3DDevice_Initialize queues a gamma update, and its first Swap
  // programs the display controller with D3D's default ramp corrected for the
  // display VdGetCurrentDisplayGamma reports (BT.709 HDTV):
  // Rec709_encode(sRGB_decode(x)). The native renderer runs neither, so
  // present applies the same curve; SYLPHEED never sets a ramp of its own.
  const float display_correction =
      REXCVAR_GET(sylpheed_display_correction) ? 1.0f : 0.0f;
  s.command_list->setPipeline(s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  // Gamma 1: D3D's ramp is linear before the display correction.
  } pc{gamma_src_desc, 0, 1.0f, display_correction};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

namespace {

// The backbuffer cleared to black with only the overlay on top. Used before
// the guest runs, and by Present while the guest has no render target the
// host can show.
void RecordOverlayOnlyPass(VideoState &s, plume::RenderTexture *back,
                           plume::RenderFramebuffer *back_fb) {
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil) {
  Clear(flags, ArgbToRenderColor(color_argb), depth, stencil);
}

void Video::Clear(u32 flags, const plume::RenderColor &color, float depth,
                  u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!BindDrawFramebufferLocked(clear_color, /*color_written=*/clear_color))
    return;
  if (clear_color)
    s.command_list->clearColor(0, color);
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth,
                                      stencil);
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::Present(GuestTexture *frontBuffer) {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  u32 texture_index = 0;
  {
    SYLPHEED_CPU_ZONE("AcquireTexture");
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  BeginCommandList(s);
  perf::GpuPassMark(s, "present");
  if (have_rt_blit) {
    RecordPresentPass(s, rt, back, back_fb);
  } else {
    // sylpheed: until the surface hooks hand the guest host render targets there
    // is nothing to blit, so keep the swapchain and overlay alive instead.
    static std::atomic<u32> s_log{0};
    if (s_log.fetch_add(1, std::memory_order_relaxed) == 0)
      SYLPHEED_WARN("Present: no drawable RT, presenting the overlay only");
    RecordOverlayOnlyPass(s, back, back_fb);
  }

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  perf::GpuListClosing(s, cur);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    SYLPHEED_CPU_ZONE("Submit");
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  {
    SYLPHEED_CPU_ZONE("PresentSwap");
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
  }
  perf::NotePresentQueued();
  {
    SYLPHEED_CPU_ZONE("WaitFence");
    perf::Scope timer(perf::Counter::FenceWait);
    AdvanceAndWaitReused(s);
  }
  MaybeDumpFrame(s, have_rt_blit ? rt : nullptr);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  SYLPHEED_FRAME_MARK();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    SYLPHEED_CPU_ZONE("DrainSlot");
    DrainSlot(s, reclaimed);
  }
  {
    SYLPHEED_CPU_ZONE("PaceFrame");
    perf::Scope timer(perf::Counter::PaceSleep);
    PaceFrame();
  }
  perf::OnPresent();
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        SYLPHEED_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  RecordOverlayOnlyPass(s, back, back_fb);

  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace sylpheed::gpu
