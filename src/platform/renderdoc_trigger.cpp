// sylpheed - ReXGlue Recompiled Project
//
// sylpheed_renderdoc_at_mission=S: under RenderDoc (renderdoccmd capture or
// qrenderdoc's Launch), capture the first frame S seconds into the mission,
// i.e. after the first 3D frame (platform/input_script.cpp). Both renderers
// call it once per present, so a scripted run of each build captures the same
// moment and their draws can be compared side by side. Nothing happens when
// RenderDoc has not injected itself.

#include <rex/cvar.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/settings.h"
#include "thirdparty/renderdoc/renderdoc_app.h"

#if defined(_WIN32)
#include "core/windows_lean.h"
#endif

REXCVAR_DEFINE_DOUBLE(sylpheed_renderdoc_at_mission, -1.0, kCvarGroup,
                      "Under RenderDoc, capture the frame this many seconds "
                      "after the mission's first 3D frame (negative = off)");

double SylpheedMissionSeconds(); // platform/input_script.cpp

namespace {

RENDERDOC_API_1_0_0 *RenderDocApi() {
  static RENDERDOC_API_1_0_0 *api = [] {
    RENDERDOC_API_1_0_0 *found = nullptr;
#if defined(_WIN32)
    if (HMODULE module = GetModuleHandleA("renderdoc.dll")) {
      auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
          GetProcAddress(module, "RENDERDOC_GetAPI"));
      if (!get_api ||
          !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void **>(&found)))
        found = nullptr;
    }
#endif
    if (!found)
      SYLPHEED_WARN("sylpheed_renderdoc_at_mission: RenderDoc is not attached");
    return found;
  }();
  return api;
}

} // namespace

// Once per game frame (the guest's Swap). The capture spans whole game frames
// between two calls rather than host presents: the xenos plugin's presenter
// repaints at the display rate on its own, and a present-to-present capture
// holds only its blit. `frames` game frames are captured; 0 takes RenderDoc's
// next present-to-present frame instead.
void SylpheedRenderDocOnPresent(u32 frames) {
  const double at = REXCVAR_GET(sylpheed_renderdoc_at_mission);
  if (at < 0.0)
    return;
  static enum { kWaiting, kCapturing, kDone } s_state = kWaiting;
  static u32 s_left = 0;
  if (s_state == kDone)
    return;
  auto *api = RenderDocApi();
  if (!api) {
    s_state = kDone;
    return;
  }
  if (s_state == kCapturing) {
    if (--s_left == 0) {
      api->EndFrameCapture(nullptr, nullptr);
      s_state = kDone;
      SYLPHEED_INFO("sylpheed_renderdoc_at_mission: capture ended");
    }
    return;
  }
  const double secs = SylpheedMissionSeconds();
  if (secs < at)
    return;
  if (frames == 0) {
    // The native renderer presents once per game frame, and records the next
    // frame's command list before this call, which a Start/End window would
    // miss: RenderDoc's own next-present capture instead.
    api->TriggerCapture();
    s_state = kDone;
    SYLPHEED_INFO("sylpheed_renderdoc_at_mission: capturing at {:.1f}s", secs);
    SYLPHEED_INFO("sylpheed_renderdoc_at_mission: capture ended");
    return;
  }
  api->StartFrameCapture(nullptr, nullptr);
  s_state = kCapturing;
  s_left = frames ? frames : 1;
  SYLPHEED_INFO("sylpheed_renderdoc_at_mission: capturing at {:.1f}s", secs);
}
