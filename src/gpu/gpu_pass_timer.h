// sylpheed - ReXGlue Recompiled Project
//
// GPU time per render pass, for sylpheed_perf_stats: a timestamp where each
// pass starts (a framebuffer bind, a resolve, the present blit), read back
// once the frame's fence has passed. A pass runs until the next mark. D3D12
// only; the calls do nothing on Vulkan, whose timestamp readback cannot take a
// partly written query pool.

#pragma once

#include <string>

#include <rex/types.h>

namespace sylpheed::gpu {

struct VideoState;

namespace perf {

// After the slot's command list begins, and just before it ends.
void GpuListOpened(VideoState &s, u32 slot);
void GpuListClosing(VideoState &s, u32 slot);
// A pass starts here. label must outlive the frame (a string literal).
void GpuPassMark(VideoState &s, const char *label, u32 width = 0, u32 height = 0);
// A draw starts here (sylpheed_perf_gpu_draws): its time is filed under its
// pixel shader and target size instead of the pass.
void GpuDrawMark(VideoState &s, u64 pixel_shader_hash, u32 width, u32 height);
// The slot's fence has passed: fold its timestamps into the totals.
void GpuSlotRetired(VideoState &s, u32 slot);
// Per-pass totals over `frames` presents, as report lines, and reset them.
std::string TakeGpuPassReport(u64 frames);

} // namespace perf
} // namespace sylpheed::gpu
