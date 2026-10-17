// sylpheed - ReXGlue Recompiled Project

#include "core/settings.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(sylpheed_devmode, false, kCvarGroup,
                    "Extra renderer diagnostics and developer logging");
REXCVAR_DEFINE_INT32(sylpheed_shutdown_timeout_ms, 5000, kCvarGroup,
                     "Milliseconds a quit may take before it is forced");

namespace sylpheed {

Settings &Settings::Get() {
  static Settings settings;
  return settings;
}

bool Settings::Devmode() const { return REXCVAR_GET(sylpheed_devmode); }

i32 Settings::ShutdownTimeoutMs() const {
  return REXCVAR_GET(sylpheed_shutdown_timeout_ms);
}

} // namespace sylpheed
