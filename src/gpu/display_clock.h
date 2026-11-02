// sylpheed - ReXGlue Recompiled Project
//
// The display the game window is on, watched from a thread of its own so no
// slow display query runs on the presenting thread. It tracks the display's
// refresh rate and timestamps its vblanks, which frame pacing uses to tell a
// fixed refresh display from a variable refresh one (FreeSync, G-Sync, HDMI
// VRR). D3D12 build only: the Vulkan build paces on the timer alone.

#pragma once

#include <chrono>
#include <vector>

#include <rex/types.h>

namespace sylpheed::gpu::display {

using Clock = std::chrono::steady_clock;

// Starts the thread for this window. Later calls do nothing.
void Start(void *hwnd);

struct Info {
  // The mode's refresh rate in whole Hz (59 for 59.94), 0 if unknown.
  u32 hz = 0;
  // Bumped whenever the window changes display or the display changes mode.
  u32 generation = 0;
  // False until the vblank clock runs. It stops while the desktop is locked.
  bool vblanks = false;
};
Info Current();

// The times between consecutive vblanks timestamped after 'since', in
// milliseconds, oldest first. Holds the last 511 at most.
std::vector<double> VBlankIntervals(Clock::time_point since);

} // namespace sylpheed::gpu::display
