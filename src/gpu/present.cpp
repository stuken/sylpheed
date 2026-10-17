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
#include "gpu/frame.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include <plume_render_interface.h>
#if defined(SYLPHEED_D3D12)
#include <plume_d3d12.h>
// After plume_d3d12.h, which brings in windows.h. winternl.h has NTSTATUS.
#include <winternl.h>
#include <d3dkmthk.h>
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
#include "gpu/output.h"
#include "gpu/settings.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_INT32(sylpheed_fps_limit, 60, kCvarGroup,
                     "Frame rate cap for presentation, 0 for none");
REXCVAR_DEFINE_BOOL(sylpheed_display_correction, true, kCvarGroup,
                    "Apply the console's HDTV display correction at present, "
                    "as D3D programs it; off shows the raw frame (brighter "
                    "mid-tones)");

namespace sylpheed::gpu {

namespace {

using Clock = std::chrono::steady_clock;

// Set while vsync alone holds the frame rate (see ApplyVsync), so PaceFrame
// only guards against presents that stop blocking.
bool g_display_locked = false;

// The window's display refresh rate in whole Hz (59 for 59.94), 0 if unknown.
// Re-read every two seconds so moving the window to another display counts.
u32 DisplayRefreshHz(VideoState &s) {
#if defined(SYLPHEED_D3D12)
  static u32 s_hz = 0;
  static Clock::time_point s_checked{};
  const auto now = Clock::now();
  if (s_checked.time_since_epoch().count() != 0 &&
      now - s_checked < std::chrono::seconds(2))
    return s_hz;
  s_checked = now;
  s_hz = 0;
  auto *chain = static_cast<plume::D3D12SwapChain *>(s.swap_chain.get());
  IDXGIOutput *output = nullptr;
  if (!chain->d3d || FAILED(chain->d3d->GetContainingOutput(&output)))
    return s_hz;
  DXGI_OUTPUT_DESC desc{};
  const bool described = SUCCEEDED(output->GetDesc(&desc));
  output->Release();
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  // 0 and 1 mean the hardware default rate, which says nothing.
  if (described &&
      EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1)
    s_hz = mode.dmDisplayFrequency;
  return s_hz;
#else
  (void)s;
  return 0;
#endif
}

// With vsync on and a refresh rate that is a whole multiple of the frame cap
// (60 or 120 Hz at 60 fps), the swap chain waits that many vblanks per frame:
// every frame stays on screen for the same number of refreshes, locked to the
// display. A sleep pacer can't do that at 120 Hz: its wake-ups wander by a
// millisecond or so, and any that straddle a vblank push a frame one refresh
// late, which judders.
void ApplyVsync(VideoState &s) {
  if (!s.swap_chain)
    return;
  const bool vsync = Settings::Get().Vsync();
  s.swap_chain->setVsyncEnabled(vsync);
  g_display_locked = false;
#if defined(SYLPHEED_D3D12)
  UINT interval = 1;
  const i32 fps = REXCVAR_GET(sylpheed_fps_limit);
  const u32 hz = DisplayRefreshHz(s);
  if (vsync && fps > 0 && hz) {
    const u32 n = (hz + u32(fps) / 2) / u32(fps);
    // DXGI takes at most 4. A 1 Hz slack covers the 59.94-style rates.
    if (n >= 1 && n <= 4 && std::abs(i32(hz) - i32(n) * fps) <= 1) {
      interval = n;
      g_display_locked = true;
    }
  }
  auto *chain = static_cast<plume::D3D12SwapChain *>(s.swap_chain.get());
  if (chain->vsyncInterval != interval) {
    SYLPHEED_INFO("frame pacing: {}", g_display_locked
                                     ? std::format("{} vblank(s) per frame at "
                                                   "{} Hz",
                                                   interval, hz)
                                     : std::string("timer"));
    chain->vsyncInterval = interval;
  }
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

#if defined(SYLPHEED_D3D12)
// For sylpheed_perf_stats: a thread timestamping the window's display vblanks, so
// each frame's time on screen can be worked out. DXGI frame statistics read
// zero while DWM composes the window, which is the usual case, and
// IDXGIOutput::WaitForVBlank returns at once on some drivers, so this waits
// on the kernel's vblank event for the window's output.
void StartVBlankClock(VideoState &s) {
  static std::once_flag s_once;
  if (!perf::Enabled())
    return;
  std::call_once(s_once, [&s] {
    auto *chain = static_cast<plume::D3D12SwapChain *>(s.swap_chain.get());
    IDXGIOutput *output = nullptr;
    const HRESULT got = chain->d3d ? chain->d3d->GetContainingOutput(&output)
                                   : E_POINTER;
    if (FAILED(got)) {
      SYLPHEED_WARN("perf stats: no vblank clock (GetContainingOutput 0x{:08X})",
               u32(got));
      return;
    }
    DXGI_OUTPUT_DESC desc{};
    const bool described = SUCCEEDED(output->GetDesc(&desc));
    output->Release();
    if (!described)
      return;
    HDC dc = CreateDCW(nullptr, desc.DeviceName, nullptr, nullptr);
    if (!dc) {
      SYLPHEED_WARN("perf stats: no vblank clock (CreateDC failed)");
      return;
    }
    D3DKMT_OPENADAPTERFROMHDC open{};
    open.hDc = dc;
    const NTSTATUS opened = D3DKMTOpenAdapterFromHdc(&open);
    DeleteDC(dc);
    if (opened != 0) {
      SYLPHEED_WARN("perf stats: no vblank clock (D3DKMTOpenAdapterFromHdc 0x{:X})",
               u32(opened));
      return;
    }
    std::thread([open] {
      D3DKMT_WAITFORVERTICALBLANKEVENT wait{};
      wait.hAdapter = open.hAdapter;
      wait.VidPnSourceId = open.VidPnSourceId;
      NTSTATUS status;
      while ((status = D3DKMTWaitForVerticalBlankEvent(&wait)) == 0)
        perf::NoteVBlank();
      SYLPHEED_WARN("perf stats: vblank clock stopped (0x{:X})", u32(status));
    }).detach();
  });
}
#endif

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
  if (!BindDrawFramebufferLocked(clear_color))
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
#if defined(SYLPHEED_D3D12)
  StartVBlankClock(s);
#endif
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
