// sylpheed - ReXGlue Recompiled Project

#include "gpu/display_clock.h"

#if defined(SYLPHEED_D3D12)

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

// The vblank wait and display queries need GDI, which core/windows_lean.h
// leaves out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h. winternl.h has NTSTATUS.
#include <winternl.h>
#include <d3dkmthk.h>

#include "core/logging.h"
#include "gpu/perf_stats.h"

namespace sylpheed::gpu::display {

namespace {

std::atomic<u32> g_hz{0};
std::atomic<u32> g_generation{0};
std::atomic<bool> g_vblanks{false};

// The most recent vblank timestamps, oldest overwritten first.
constexpr std::size_t kStamps = 512;
std::mutex g_stamp_mutex;
std::array<Clock::time_point, kStamps> g_stamps{};
std::size_t g_stamp_count = 0; // total written; the newest is at count - 1

void NoteStamp(Clock::time_point t) {
  std::lock_guard lock(g_stamp_mutex);
  g_stamps[g_stamp_count % kStamps] = t;
  ++g_stamp_count;
}

// The window's display: its GDI device name and current refresh rate.
struct Monitor {
  HMONITOR handle = nullptr;
  WCHAR device[CCHDEVICENAME] = {};
  u32 hz = 0;
};

bool QueryMonitor(HWND hwnd, Monitor &out) {
  out.handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!out.handle)
    return false;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(out.handle, &info))
    return false;
  std::copy(std::begin(info.szDevice), std::end(info.szDevice),
            std::begin(out.device));
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  // 0 and 1 mean the hardware default rate, which says nothing.
  out.hz = EnumDisplaySettingsW(out.device, ENUM_CURRENT_SETTINGS, &mode) &&
                   mode.dmDisplayFrequency > 1
               ? mode.dmDisplayFrequency
               : 0;
  return true;
}

struct Adapter {
  D3DKMT_HANDLE handle = 0;
  D3DDDI_VIDEO_PRESENT_SOURCE_ID source = 0;

  bool Open(const WCHAR *device) {
    Close();
    HDC dc = CreateDCW(nullptr, device, nullptr, nullptr);
    if (!dc)
      return false;
    D3DKMT_OPENADAPTERFROMHDC open{};
    open.hDc = dc;
    const NTSTATUS opened = D3DKMTOpenAdapterFromHdc(&open);
    DeleteDC(dc);
    if (opened != 0)
      return false;
    handle = open.hAdapter;
    source = open.VidPnSourceId;
    return true;
  }
  void Close() {
    if (!handle)
      return;
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = handle;
    D3DKMTCloseAdapter(&close);
    handle = 0;
  }
};

// Re-reads the display at most this often: EnumDisplaySettingsW can take
// milliseconds. A move to another display is caught on the next vblank.
constexpr auto kModeRecheck = std::chrono::seconds(1);

void Run(HWND hwnd) {
  Monitor current;
  Adapter adapter;
  Clock::time_point checked{};
  bool warned = false;
  for (;;) {
    if (!IsWindow(hwnd)) {
      adapter.Close();
      g_vblanks.store(false, std::memory_order_relaxed);
      return;
    }
    const auto now = Clock::now();
    const HMONITOR handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (handle != current.handle || now - checked >= kModeRecheck) {
      checked = now;
      Monitor next;
      if (QueryMonitor(hwnd, next)) {
        const bool moved = next.handle != current.handle ||
                           std::wcscmp(next.device, current.device) != 0;
        if (moved || next.hz != current.hz) {
          if (moved)
            adapter.Close();
          current = next;
          g_hz.store(current.hz, std::memory_order_relaxed);
          g_generation.fetch_add(1, std::memory_order_release);
        }
      }
    }
    if (!adapter.handle && current.handle && !adapter.Open(current.device)) {
      if (!warned)
        SYLPHEED_WARN("display clock: can't open the display's adapter, so frame "
                 "pacing can't check for variable refresh");
      warned = true;
      g_vblanks.store(false, std::memory_order_relaxed);
      Sleep(500);
      continue;
    }
    D3DKMT_WAITFORVERTICALBLANKEVENT wait{};
    wait.hAdapter = adapter.handle;
    wait.VidPnSourceId = adapter.source;
    if (D3DKMTWaitForVerticalBlankEvent(&wait) != 0) {
      // STATUS_GRAPHICS_PRESENT_OCCLUDED while the desktop is locked or the
      // display is off, or the adapter went away. Retry from a fresh open.
      g_vblanks.store(false, std::memory_order_relaxed);
      adapter.Close();
      Sleep(250);
      continue;
    }
    const auto stamp = Clock::now();
    g_vblanks.store(true, std::memory_order_relaxed);
    NoteStamp(stamp);
    if (perf::Enabled())
      perf::NoteVBlank();
  }
}

} // namespace

void Start(void *hwnd) {
  static std::once_flag s_once;
  if (!hwnd)
    return;
  std::call_once(s_once, [hwnd] {
    HWND window = static_cast<HWND>(hwnd);
    // Publish the refresh rate before the first frame paces on it.
    Monitor first;
    if (QueryMonitor(window, first)) {
      g_hz.store(first.hz, std::memory_order_relaxed);
      g_generation.fetch_add(1, std::memory_order_release);
    }
    std::thread([window] { Run(window); }).detach();
  });
}

Info Current() {
  Info info;
  info.generation = g_generation.load(std::memory_order_acquire);
  info.hz = g_hz.load(std::memory_order_relaxed);
  info.vblanks = g_vblanks.load(std::memory_order_relaxed);
  return info;
}

std::vector<double> VBlankIntervals(Clock::time_point since) {
  std::vector<double> intervals;
  {
    std::lock_guard lock(g_stamp_mutex);
    const std::size_t held = std::min(g_stamp_count, kStamps);
    const std::size_t first = g_stamp_count - held;
    intervals.reserve(held);
    for (std::size_t i = first + 1; i < g_stamp_count; ++i) {
      const auto a = g_stamps[(i - 1) % kStamps];
      const auto b = g_stamps[i % kStamps];
      if (a <= since)
        continue;
      intervals.push_back(
          std::chrono::duration<double, std::milli>(b - a).count());
    }
  }
  return intervals;
}

} // namespace sylpheed::gpu::display

#else

namespace sylpheed::gpu::display {
void Start(void *) {}
Info Current() { return {}; }
std::vector<double> VBlankIntervals(Clock::time_point) { return {}; }
} // namespace sylpheed::gpu::display

#endif
