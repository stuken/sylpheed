// sylpheed - ReXGlue Recompiled Project

#include "gpu/perf_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"

REXCVAR_DEFINE_INT32(sylpheed_perf_stats, 0, kCvarGroup,
                     "Log frame and renderer hot-path timings every N "
                     "presents (0 = off)");

namespace sylpheed::gpu::perf {

namespace {

struct Stat {
  std::atomic<u64> count{0};
  std::atomic<u64> ns{0};
  std::atomic<u64> max_ns{0};
  std::atomic<u64> bytes{0};
};

std::array<Stat, size_t(Counter::kCount)> g_stats;

constexpr const char *kNames[] = {
    "pso build", "shader link", "texture mirror", "texel hash", "buffer hash",
    "buffer upload", "draw", "fence wait", "pace sleep",
};
static_assert(std::size(kNames) == size_t(Counter::kCount));

// Present runs on one thread at a time (under the video state mutex's
// ordering), so the interval history needs no lock.
std::vector<double> g_intervals_ms;
std::chrono::steady_clock::time_point g_last_present{};
u64 g_presents = 0;

using Clock = std::chrono::steady_clock;

std::mutex g_vblank_mutex;
std::deque<Clock::time_point> g_vblanks;
std::vector<Clock::time_point> g_present_times;

// Refreshes between consecutive presents' first vblanks: [0] (two presents
// in one refresh, one never shown), [1], [2], [3], [4+].
std::array<u32, 5> RefreshHistogram() {
  std::array<u32, 5> h{};
  std::lock_guard lock(g_vblank_mutex);
  i64 prev = -1;
  size_t v = 0;
  for (const auto &t : g_present_times) {
    while (v < g_vblanks.size() && g_vblanks[v] <= t)
      ++v;
    if (v == g_vblanks.size())
      break; // not displayed yet
    if (prev >= 0)
      ++h[std::min<i64>(i64(v) - prev, 4)];
    prev = i64(v);
  }
  // Keep the vblanks the next window's first present can still land on.
  while (g_vblanks.size() > 8)
    g_vblanks.pop_front();
  return h;
}

std::string FormatBytes(u64 bytes) {
  if (bytes >= (1ull << 20))
    return std::format("{:.1f} MiB", double(bytes) / double(1ull << 20));
  if (bytes >= (1ull << 10))
    return std::format("{:.1f} KiB", double(bytes) / double(1ull << 10));
  return std::format("{} B", bytes);
}

} // namespace

bool Enabled() { return REXCVAR_GET(sylpheed_perf_stats) > 0; }

void Add(Counter counter, u64 ns, u64 bytes) {
  Stat &s = g_stats[size_t(counter)];
  s.count.fetch_add(1, std::memory_order_relaxed);
  s.ns.fetch_add(ns, std::memory_order_relaxed);
  s.bytes.fetch_add(bytes, std::memory_order_relaxed);
  u64 prev = s.max_ns.load(std::memory_order_relaxed);
  while (ns > prev &&
         !s.max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
  }
}

void NoteVBlank() {
  const auto now = Clock::now();
  std::lock_guard lock(g_vblank_mutex);
  g_vblanks.push_back(now);
  if (g_vblanks.size() > 100000)
    g_vblanks.pop_front();
}

void NotePresentQueued() {
  if (!Enabled())
    return;
  const auto now = Clock::now();
  std::lock_guard lock(g_vblank_mutex);
  g_present_times.push_back(now);
}

void OnPresent() {
  const i32 every = REXCVAR_GET(sylpheed_perf_stats);
  if (every <= 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  ++g_presents;
  if (g_last_present.time_since_epoch().count() != 0) {
    const double ms =
        std::chrono::duration<double, std::milli>(now - g_last_present).count();
    g_intervals_ms.push_back(ms);
    if (ms > 50.0)
      SYLPHEED_INFO("[perf] long frame: present {} took {:.1f} ms", g_presents, ms);
  }
  g_last_present = now;
  if (g_intervals_ms.size() < size_t(every))
    return;

  std::vector<double> sorted = g_intervals_ms;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0;
  for (double v : sorted)
    sum += v;
  const size_t n = sorted.size();
  auto pct = [&](double p) { return sorted[std::min(n - 1, size_t(p * n))]; };
  u32 over_17 = 0, over_33 = 0;
  for (double v : sorted) {
    over_17 += v > 17.5;
    over_33 += v > 34.0;
  }
  SYLPHEED_INFO("[perf] presents {}-{}: frame avg {:.2f} ms, p50 {:.2f}, p99 "
           "{:.2f}, max {:.2f}; {} over 17.5 ms, {} over 34 ms",
           g_presents - n, g_presents, sum / double(n), pct(0.5), pct(0.99),
           sorted.back(), over_17, over_33);

  std::string line;
  for (size_t i = 0; i < size_t(Counter::kCount); ++i) {
    Stat &s = g_stats[i];
    const u64 count = s.count.exchange(0, std::memory_order_relaxed);
    const u64 ns = s.ns.exchange(0, std::memory_order_relaxed);
    const u64 max_ns = s.max_ns.exchange(0, std::memory_order_relaxed);
    const u64 bytes = s.bytes.exchange(0, std::memory_order_relaxed);
    if (!count)
      continue;
    line += std::format("\n    {:<14} {:>7} x, {:8.2f} ms/frame, max {:7.2f} "
                        "ms",
                        kNames[i], count, double(ns) / 1e6 / double(n),
                        double(max_ns) / 1e6);
    if (bytes)
      line += std::format(", {}/frame", FormatBytes(bytes / n));
  }
  if (!line.empty())
    SYLPHEED_INFO("[perf] per counter:{}", line);
  const auto h = RefreshHistogram();
  if (h[0] + h[1] + h[2] + h[3] + h[4]) {
    double refresh_ms = 0;
    {
      std::lock_guard lock(g_vblank_mutex);
      if (g_vblanks.size() > 1)
        refresh_ms = std::chrono::duration<double, std::milli>(
                         g_vblanks.back() - g_vblanks.front())
                         .count() /
                     double(g_vblanks.size() - 1);
    }
    SYLPHEED_INFO("[perf] refreshes per frame ({:.3f} ms refresh): 0: {}, 1: {}, "
             "2: {}, 3: {}, 4+: {}",
             refresh_ms, h[0], h[1], h[2], h[3], h[4]);
  }
  {
    std::lock_guard lock(g_vblank_mutex);
    // The last present may not have reached a vblank yet: carry it over.
    if (!g_present_times.empty())
      g_present_times.erase(g_present_times.begin(),
                            g_present_times.end() - 1);
  }
  g_intervals_ms.clear();
}

} // namespace sylpheed::gpu::perf
