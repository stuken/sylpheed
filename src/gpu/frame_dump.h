// sylpheed - ReXGlue Recompiled Project

#pragma once

namespace sylpheed::gpu {

struct VideoState;
struct GuestTexture;

// Called once per presented frame with the frame's source render target, after
// its command list was submitted. Dumps it when sylpheed_dump_frames names this
// frame. Caller holds s.mutex.
void MaybeDumpFrame(VideoState &s, GuestTexture *rt);

} // namespace sylpheed::gpu
