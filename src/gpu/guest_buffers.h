// sylpheed - ReXGlue Recompiled Project
//
// Host mirrors of guest-owned vertex and index buffers. SYLPHEED's D3D still
// creates and owns its buffers (the resource hooks are not ported), so draws
// mirror whatever range the device has bound: keyed by guest base address,
// rehashed once per frame on first use, and re-uploaded only when the bytes
// changed. That covers static meshes, CPU-rewritten dynamic buffers and
// headers the game builds itself with no lifecycle hooks at all.

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace sylpheed::gpu {

struct GuestBuffer;
enum class ResourceType : u32;

// The host mirror of [base_va, base_va + size). element_size picks the byte
// swap (4 for vertex data, 2 or 4 for indices). nullptr on failure.
GuestBuffer *MirrorGuestBuffer(u32 base_va, u32 size, ResourceType type,
                               u32 element_size, plume::RenderFormat format);

// Drops the mirror of a destroyed guest buffer (D3DResource_Destroy), so its
// host memory is freed and a later buffer at the same address starts fresh.
// Bindings that still name it are cleared. Returns whether one existed.
bool EvictGuestBuffer(u32 base_va, ResourceType type);

// EvictGuestBuffer for every mirror whose base lies in [begin, end), a freed
// XPhysicalAlloc range (virtual, like the buffer headers). Returns the count.
u32 EvictGuestBuffersInRange(u32 begin, u32 end);

// Marks a new frame, so every mirror is rehashed on its next use.
void AdvanceGuestBufferFrame();

// The frame counter AdvanceGuestBufferFrame advances, for other per-frame
// mirror checks.
u64 GuestMirrorFrame();

// Frees mirrors replaced while frame slot 'slot' was recording, after its
// fence, like the other per-slot graveyards.
void DrainGuestBufferMirrors(u32 slot);

} // namespace sylpheed::gpu
