// sylpheed - ReXGlue Recompiled Project
//
// RenderDoc's in-application API, for capturing one chosen frame of a run
// launched under RenderDoc (renderdoccmd capture, or qrenderdoc's Launch).
// sylpheed_renderdoc_capture captures the frame sylpheed_trace_frame names,
// so a scripted run can capture the frame it traces. Nothing happens when
// RenderDoc has not injected itself.

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"
#include "thirdparty/renderdoc/renderdoc_app.h"

#if defined(_WIN32)
#include "core/windows_lean.h"
#endif

REXCVAR_DEFINE_BOOL(sylpheed_renderdoc_capture, false, kCvarGroup,
                    "Under RenderDoc, capture the frame sylpheed_trace_frame "
                    "names");

namespace sylpheed::gpu {

u32 TraceFrameNumber();

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
      SYLPHEED_WARN("sylpheed_renderdoc_capture: RenderDoc is not attached");
    return found;
  }();
  return api;
}

} // namespace

// After present `presented`: RenderDoc captures from the next present on, so
// the trigger goes in one present before the traced frame.
void MaybeTriggerRenderDocCapture(u32 presented) {
  if (!REXCVAR_GET(sylpheed_renderdoc_capture))
    return;
  const u32 frame = TraceFrameNumber();
  if (!frame || presented + 1 != frame)
    return;
  if (auto *api = RenderDocApi()) {
    api->TriggerCapture();
    SYLPHEED_INFO("sylpheed_renderdoc_capture: capturing frame {}", frame);
  }
}

} // namespace sylpheed::gpu
