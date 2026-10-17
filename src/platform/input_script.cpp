// sylpheed - ReXGlue Recompiled Project
//
// sylpheed_input_script: scripted controller input for unattended testing, e.g.
//   --sylpheed_input_script="start@900,a@1000+5,rt@1100+600"
// Each entry presses a button (or pulls a trigger) on present frame N and
// holds it for the given number of frames (default 6). A trailing 's' on
// either number counts seconds since the game first polled the pad instead,
// and a trailing 'w' seconds since the first 3D frame (the mission start);
// 'w' entries wait for it.
//
// The entry "mission" plays the game from boot into the first mission and
// stops pressing once its first 3D frame renders: START through the logos and
// opening movie, A through the title, menus and difficulty, then a repeating
// Ready Room macro until the first 3D frame (A into the required briefing, START to skip it, UP to
// TAKE OFF once it enables about two seconds later, A), and the two A presses
// the 3D launch intro wants (2 and 7 seconds into it). E.g.
//   --sylpheed_input_script="mission,rt@5w+300"
// The xenos plugin build counts frames by wrapping the guest's Present and
// logs every 300th, so captures can be timed against the native renderer's
// frame numbers. Buttons: a b x y start
// back up down left right lb rb ls rs; triggers: lt rt. Applied to user 0 on
// top of whatever a real pad reports, and while a script is set a missing pad
// reads as connected, since remote sessions usually have none.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_STRING(sylpheed_input_script, "", kCvarGroup,
                      "Scripted pad input for testing: button@frame[+hold],...");

// Unattended runs over remote sessions can lose the audio endpoint, and the
// guest faults at boot when audio init fails. This swaps the SDL
// backend for a silent one (platform/silent_audio.h, read in SylpheedApp::OnPreSetup).
REXCVAR_DEFINE_BOOL(sylpheed_null_audio, false, kCvarGroup,
                    "Discard audio output (testing without an audio device)");

REX_EXTERN(__imp__XInputGetState);
REX_EXTERN(__imp__XInputGetKeystroke);
REX_EXTERN(__imp__Sylpheed_RenderScene);

#if defined(SYLPHEED_NATIVE_GPU)
namespace sylpheed::gpu {
u32 PresentCount();
}
#else
REX_EXTERN(__imp__D3DDevice_Swap);

namespace {
std::atomic<u32> g_presents{0};
}

void SylpheedXenosDumpOnPresent(); // platform/xenos_resolve_dump.cpp
void SylpheedRenderDocOnPresent(u32 frames); // platform/renderdoc_trigger.cpp

// Sylpheed's main loop swaps directly, and D3DDevice_Present ends in Swap
// too, so every displayed frame passes through here.
REX_HOOK_RAW(D3DDevice_Swap) {
  const u32 n = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 300 == 0)
    SYLPHEED_INFO("sylpheed_input_script: present {}", n);
  SylpheedXenosDumpOnPresent();
  // Two game frames: the plugin's GPU thread executes behind the guest.
  SylpheedRenderDocOnPresent(2);
  __imp__D3DDevice_Swap(ctx, base);
}
#endif

namespace {

enum class Clock { kFrames, kSeconds, kWorldSeconds };

struct Press {
  u16 buttons = 0;
  u8 left_trigger = 0;
  u8 right_trigger = 0;
  double start = 0; // in `clock` units
  double hold = 6;
  Clock clock = Clock::kFrames;
};

// Seconds since the game first polled the pad.
double ScriptSeconds() {
  using SteadyClock = std::chrono::steady_clock;
  static const SteadyClock::time_point t0 = SteadyClock::now();
  return std::chrono::duration<double>(SteadyClock::now() - t0).count();
}

// ScriptSeconds() at the first 3D frame, negative until then.
std::atomic<double> g_world_start{-1.0};
bool g_mission_macro = false;

// XINPUT_STATE.
struct InputState {
  be_u32 dwPacketNumber;
  be_u16 wButtons;
  u8 bLeftTrigger;
  u8 bRightTrigger;
  be_i16 sThumbLX;
  be_i16 sThumbLY;
  be_i16 sThumbRX;
  be_i16 sThumbRY;
};

bool ParseButton(const std::string &name, Press &p) {
  static constexpr struct {
    const char *name;
    u16 mask;
  } kButtons[] = {{"up", 0x0001},     {"down", 0x0002}, {"left", 0x0004},
                  {"right", 0x0008},  {"start", 0x0010}, {"back", 0x0020},
                  {"ls", 0x0040},     {"rs", 0x0080},   {"lb", 0x0100},
                  {"rb", 0x0200},     {"a", 0x1000},    {"b", 0x2000},
                  {"x", 0x4000},      {"y", 0x8000}};
  for (const auto &b : kButtons) {
    if (name == b.name) {
      p.buttons = b.mask;
      return true;
    }
  }
  if (name == "lt") {
    p.left_trigger = 255;
    return true;
  }
  if (name == "rt") {
    p.right_trigger = 255;
    return true;
  }
  return false;
}

const std::vector<Press> &Script() {
  static std::once_flag once;
  static std::vector<Press> presses;
  std::call_once(once, [] {
    const std::string text = REXCVAR_GET(sylpheed_input_script);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find(',', pos);
      if (end == std::string::npos)
        end = text.size();
      const std::string entry = text.substr(pos, end - pos);
      pos = end + 1;
      if (entry == "mission") {
        g_mission_macro = true;
        continue;
      }
      const size_t at = entry.find('@');
      if (at == std::string::npos)
        continue;
      Press p;
      if (!ParseButton(entry.substr(0, at), p)) {
        SYLPHEED_WARN("sylpheed_input_script: unknown button in '{}'", entry);
        continue;
      }
      const size_t plus = entry.find('+', at);
      const std::string when = entry.substr(at + 1, plus - at - 1);
      const char unit = when.empty() ? '\0' : when.back();
      p.clock = unit == 's'   ? Clock::kSeconds
                : unit == 'w' ? Clock::kWorldSeconds
                              : Clock::kFrames;
      p.start = std::stod(when);
      if (plus != std::string::npos)
        p.hold = std::stod(entry.substr(plus + 1));
      else if (p.clock != Clock::kFrames)
        p.hold = 0.2;
      presses.push_back(p);
    }
    if (!presses.empty() || g_mission_macro)
      SYLPHEED_INFO("sylpheed_input_script: {} scripted press(es){}", presses.size(),
                    g_mission_macro ? " after the mission macro" : "");
  });
  return presses;
}

bool ScriptActive() { return !Script().empty() || g_mission_macro; }

// The "mission" macro's buttons at `secs`, until the first 3D frame.
u16 MissionMacro(double secs) {
  constexpr double kHold = 0.2;
  auto pressed = [&](double at) { return secs >= at && secs < at + kHold; };
  // Logos and the opening movie skip on START (as a keystroke).
  if (secs < 11.0)
    return (pressed(6.0) || pressed(8.0) || pressed(10.0)) ? 0x0010 : 0;
  // Title (PRESS A), main menu (NEW GAME), difficulty (NORMAL) and the
  // chapter card take A.
  if (secs < 22.0) {
    const double t = secs - 11.0;
    return std::fmod(t, 1.5) < kHold ? 0x1000 : 0;
  }
  // The Ready Room opens on BRIEFINGS with TAKE OFF disabled until a briefing
  // has been seen. A enters it (more A pages through), START skips the rest
  // and returns to the Ready Room, and TAKE OFF accepts UP about two seconds
  // after that. TAKE OFF plays a video that wants A and another A five
  // seconds later. Repeats until the mission starts, from wherever the menus
  // happened to be when the first cycle began.
  const double t = std::fmod(secs - 22.0, 20.0);
  auto at = [&](double offset) { return t >= offset && t < offset + kHold; };
  if (at(0.0) || at(1.5) || at(3.0) || at(9.5) || at(12.0) || at(17.0))
    return 0x1000; // A
  if (at(5.0))
    return 0x0010; // START
  if (at(8.0))
    return 0x0001; // DPAD_UP
  return 0;
}

struct Scripted {
  u16 buttons = 0;
  u8 lt = 0, rt = 0;
  double secs = 0;
};

// What the script holds down right now. Seconds count from the game's first
// pad poll.
Scripted ScriptedNow() {
  Scripted out;
  out.secs = ScriptSeconds();
#if defined(SYLPHEED_NATIVE_GPU)
  const double frame = sylpheed::gpu::PresentCount();
#else
  const double frame = g_presents.load(std::memory_order_relaxed);
#endif
  const double world_start = g_world_start.load(std::memory_order_relaxed);
  if (g_mission_macro) {
    if (world_start < 0.0) {
      out.buttons |= MissionMacro(out.secs);
    } else {
      // The launch intro after TAKE OFF already renders in 3D, and wants A
      // and another A five seconds later before the mission proper.
      const double w = out.secs - world_start;
      if ((w >= 2.0 && w < 2.2) || (w >= 7.0 && w < 7.2))
        out.buttons |= 0x1000;
    }
  }
  for (const Press &p : Script()) {
    double now = frame;
    if (p.clock == Clock::kSeconds) {
      now = out.secs;
    } else if (p.clock == Clock::kWorldSeconds) {
      if (world_start < 0.0)
        continue;
      now = out.secs - world_start;
    }
    if (now >= p.start && now < p.start + p.hold) {
      out.buttons |= p.buttons;
      out.lt = std::max(out.lt, p.left_trigger);
      out.rt = std::max(out.rt, p.right_trigger);
    }
  }
  return out;
}

// XINPUT_KEYSTROKE.
struct Keystroke {
  be_u16 VirtualKey;
  be_u16 Unicode;
  be_u16 Flags;
  u8 UserIndex;
  u8 HidCode;
};
static_assert(sizeof(Keystroke) == 8);

constexpr u32 kErrorEmpty = 4306; // ERROR_EMPTY: no keystroke queued
constexpr u16 kKeystrokeDown = 0x1, kKeystrokeUp = 0x2;

// VK_PAD_* for each wButtons bit, triggers as bits 16 and 17.
u16 PadVirtualKey(u32 bit) {
  switch (bit) {
  case 0x0001: return 0x5810; // DPAD_UP
  case 0x0002: return 0x5811; // DPAD_DOWN
  case 0x0004: return 0x5812; // DPAD_LEFT
  case 0x0008: return 0x5813; // DPAD_RIGHT
  case 0x0010: return 0x5814; // START
  case 0x0020: return 0x5815; // BACK
  case 0x0040: return 0x5816; // LTHUMB_PRESS
  case 0x0080: return 0x5817; // RTHUMB_PRESS
  case 0x0100: return 0x5805; // LSHOULDER
  case 0x0200: return 0x5804; // RSHOULDER
  case 0x1000: return 0x5800; // A
  case 0x2000: return 0x5801; // B
  case 0x4000: return 0x5802; // X
  case 0x8000: return 0x5803; // Y
  case 0x10000: return 0x5806; // LTRIGGER
  case 0x20000: return 0x5807; // RTRIGGER
  default: return 0;
  }
}

// Key-down/up events for scripted transitions, in order. XInputGetState alone
// is not enough: Sylpheed's pad class (0x82457838) also drains
// XInputGetKeystroke every poll, and what reads that queue (the opening
// movie's skip among it) never sees a press that only shows in the state.
std::mutex g_key_mutex;
u32 g_key_held = 0;
std::vector<std::pair<u16, u16>> g_key_queue; // (virtual key, flags)

void QueueScriptedTransitions(const Scripted &now) {
  const u32 held = now.buttons | (now.lt ? 0x10000u : 0u) | (now.rt ? 0x20000u : 0u);
  std::lock_guard lock(g_key_mutex);
  const u32 changed = held ^ g_key_held;
  for (u32 bit = 1; bit <= 0x20000; bit <<= 1) {
    if (!(changed & bit))
      continue;
    if (const u16 vk = PadVirtualKey(bit))
      g_key_queue.emplace_back(vk, (held & bit) ? kKeystrokeDown : kKeystrokeUp);
  }
  g_key_held = held;
}

} // namespace

// (user, reserved, keystroke) -> ERROR_SUCCESS / ERROR_EMPTY /
// ERROR_DEVICE_NOT_CONNECTED. Real keystrokes come first; scripted ones fill
// in when the pad has none queued.
REX_HOOK_RAW(XInputGetKeystroke) {
  const u32 user = ctx.r3.u32;
  const u32 keystroke_va = ctx.r5.u32;
  __imp__XInputGetKeystroke(ctx, base);
  if (!ScriptActive() || (user != 0 && user != 0xFF) || ctx.r3.u32 == 0)
    return;
  QueueScriptedTransitions(ScriptedNow());
  auto *key = sylpheed::mem::at<Keystroke>(keystroke_va);
  if (!key)
    return;
  std::lock_guard lock(g_key_mutex);
  if (g_key_queue.empty()) {
    // A missing pad would report not-connected; with a script it is a pad
    // with nothing queued.
    ctx.r3.u64 = kErrorEmpty;
    return;
  }
  const auto [vk, flags] = g_key_queue.front();
  g_key_queue.erase(g_key_queue.begin());
  key->VirtualKey = vk;
  key->Unicode = 0;
  key->Flags = flags;
  key->UserIndex = 0;
  key->HidCode = 0;
  ctx.r3.u64 = 0;
}

// (user, state) -> ERROR_SUCCESS / ERROR_DEVICE_NOT_CONNECTED
REX_HOOK_RAW(XInputGetState) {
  const u32 user = ctx.r3.u32;
  const u32 state_va = ctx.r4.u32;
  __imp__XInputGetState(ctx, base);
  if (!ScriptActive() || user != 0)
    return;
  auto *state = sylpheed::mem::at<InputState>(state_va);
  if (!state)
    return;
  if (ctx.r3.u32 != 0) {
    // No pad: report a neutral connected one so the script has a target.
    std::memset(state, 0, sizeof(InputState));
    ctx.r3.u64 = 0;
  }
  const Scripted now = ScriptedNow();
  QueueScriptedTransitions(now);
  const u16 buttons = now.buttons;
  const u8 lt = now.lt, rt = now.rt;
  const double secs = now.secs;
  if (!buttons && !lt && !rt)
    return;
  state->wButtons = u16(state->wButtons) | buttons;
  state->bLeftTrigger = std::max<u8>(state->bLeftTrigger, lt);
  state->bRightTrigger = std::max<u8>(state->bRightTrigger, rt);
  state->dwPacketNumber =
      u32(state->dwPacketNumber) + 1 + static_cast<u32>(secs * 60.0);
}

// Seconds since the first 3D frame, negative before it. Frame dumps use it
// for their 'w' entries, so both renderers dump the same moment.
double SylpheedMissionSeconds() {
  const double start = g_world_start.load(std::memory_order_relaxed);
  return start < 0.0 ? -1.0 : ScriptSeconds() - start;
}

// The 3D frame. Its first call is the mission start: the "mission" macro
// stops there and 'w' entries count from it.
REX_HOOK_RAW(Sylpheed_RenderScene) {
  double expected = -1.0;
  const double now = ScriptSeconds();
  if (g_world_start.compare_exchange_strong(expected, now,
                                            std::memory_order_relaxed)) {
#if defined(SYLPHEED_NATIVE_GPU)
    const u32 frame = sylpheed::gpu::PresentCount();
#else
    const u32 frame = g_presents.load(std::memory_order_relaxed);
#endif
    SYLPHEED_INFO("sylpheed_input_script: mission started at {:.1f}s (present {})",
                  now, frame);
  }
  __imp__Sylpheed_RenderScene(ctx, base);
}
