// sylpheed - ReXGlue Recompiled Project

#include "gpu/gpu_pass_timer.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include <unordered_map>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "gpu/device.h"
#include "core/settings.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_BOOL(sylpheed_perf_gpu_draws, false, kCvarGroup,
                    "With sylpheed_perf_stats, time every draw on the GPU and "
                    "report it by pixel shader instead of by pass");

namespace sylpheed::gpu::perf {

namespace {

// Marks per frame: a mission frame binds about 60 targets, resolves 30 and,
// timed per draw, draws about 430 times.
constexpr u32 kMaxMarks = 4096;

struct Mark {
  const char *label;
  u32 width;
  u32 height;
};

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<Mark> marks;
  bool open = false;
};

Slot g_slots[kNumFrames];

struct Total {
  u64 ns = 0;
  u64 count = 0;
};

// (label, width, height) -> GPU time. Retirement runs on the presenting
// thread, the report on the same thread from OnPresent, but keep it safe.
std::mutex g_totals_mutex;
std::map<std::tuple<const char *, u32, u32>, Total> g_totals;
u64 g_frame_ns = 0;

bool Active(VideoState &s) {
#if defined(SYLPHEED_D3D12)
  return Enabled() && s.device && s.command_list;
#else
  (void)s;
  return false;
#endif
}

void WriteMark(VideoState &s, Slot &slot, const char *label, u32 w, u32 h) {
  if (!slot.open || slot.marks.size() >= kMaxMarks)
    return;
  s.command_list->writeTimestamp(slot.pool.get(), u32(slot.marks.size()));
  slot.marks.push_back({label, w, h});
}

} // namespace

void GpuListOpened(VideoState &s, u32 slot_index) {
  Slot &slot = g_slots[slot_index];
  slot.marks.clear();
  slot.open = false;
  if (!Active(s))
    return;
  if (!slot.pool)
    slot.pool = s.device->createQueryPool(kMaxMarks);
  if (!slot.pool)
    return;
  slot.open = true;
  WriteMark(s, slot, "list start (uploads, barriers)", 0, 0);
}

void GpuListClosing(VideoState &s, u32 slot_index) {
  Slot &slot = g_slots[slot_index];
  WriteMark(s, slot, nullptr, 0, 0); // the end of the last pass
  slot.open = false;
}

void GpuPassMark(VideoState &s, const char *label, u32 width, u32 height) {
  const u32 slot_index = s.frame.load(std::memory_order_relaxed);
  WriteMark(s, g_slots[slot_index], label, width, height);
}

void GpuDrawMark(VideoState &s, u64 pixel_shader_hash, u32 width, u32 height) {
  if (!REXCVAR_GET(sylpheed_perf_gpu_draws))
    return;
  // Labels must outlive the frame: one per shader, kept for the session.
  static std::unordered_map<u64, std::string> s_labels;
  auto [it, added] = s_labels.try_emplace(pixel_shader_hash);
  if (added)
    it->second = std::format("ps {:016X}", pixel_shader_hash);
  const u32 slot_index = s.frame.load(std::memory_order_relaxed);
  WriteMark(s, g_slots[slot_index], it->second.c_str(), width, height);
}

void GpuSlotRetired(VideoState &s, u32 slot_index) {
  (void)s;
  Slot &slot = g_slots[slot_index];
  if (slot.marks.size() < 2 || !slot.pool) {
    slot.marks.clear();
    return;
  }
  slot.pool->queryResults();
  const u64 *t = slot.pool->getResults();
  std::lock_guard lock(g_totals_mutex);
  for (size_t i = 0; i + 1 < slot.marks.size(); ++i) {
    const Mark &m = slot.marks[i];
    if (!m.label || t[i + 1] < t[i])
      continue;
    Total &total = g_totals[{m.label, m.width, m.height}];
    total.ns += t[i + 1] - t[i];
    ++total.count;
  }
  if (t[slot.marks.size() - 1] >= t[0])
    g_frame_ns += t[slot.marks.size() - 1] - t[0];
  slot.marks.clear();
}

std::string TakeGpuPassReport(u64 frames) {
  std::lock_guard lock(g_totals_mutex);
  if (g_totals.empty() || !frames)
    return {};
  std::vector<std::pair<std::tuple<const char *, u32, u32>, Total>> rows(
      g_totals.begin(), g_totals.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
  std::string out = std::format("\n    GPU {:.2f} ms/frame", double(g_frame_ns) / 1e6 / double(frames));
  u32 shown = 0;
  for (const auto &[key, total] : rows) {
    if (++shown > (REXCVAR_GET(sylpheed_perf_gpu_draws) ? 30u : 14u))
      break;
    const auto &[label, w, h] = key;
    const std::string what =
        w ? std::format("{} {}x{}", label, w, h) : std::string(label);
    out += std::format("\n      {:<34} {:6.2f} ms/frame, {:6.1f} x/frame", what,
                       double(total.ns) / 1e6 / double(frames),
                       double(total.count) / double(frames));
  }
  g_totals.clear();
  g_frame_ns = 0;
  return out;
}

} // namespace sylpheed::gpu::perf
