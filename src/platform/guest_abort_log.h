// sylpheed - ReXGlue Recompiled Project

#pragma once

namespace sylpheed::platform {

// Logs the aborting thread's guest registers when the SDK's REX_FATAL calls
// std::abort, so a fatal raised from recompiled code (an indirect call to an
// unregistered address, say) names the guest call site through lr.
void InstallGuestAbortLogger();

} // namespace sylpheed::platform
