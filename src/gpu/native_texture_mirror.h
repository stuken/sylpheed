/**
 * @file  gpu/native_texture_mirror.h
 * @brief   Host mirrors of guest textures and resolve targets, keyed by guest
 *          VA. SYLPHEED creates its textures without a hooked D3D create call, so
 *          a texture is mirrored the first time a draw binds it: the Xenos
 *          fetch constant is decoded, the texels untiled and a host texture
 *          built. D3DResource_Destroy and XPhysicalFree evict.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>
#include <vector>

#include <rex/types.h>

#include "gpu/resources.h"

namespace sylpheed::gpu {

// Builds the host mirror of a guest texture, replacing any cached one at the
// same VA. nullptr when the format or dimension is unsupported or the build
// fails.
GuestTexture *GetOrCreateNativeMirror(u32 guest_va);

// SYLPHEED: the mirror of an unhooked guest texture, built on first use and rebuilt
// when its fetch constant changes. nullptr when unsupported.
GuestTexture *ResolveOrMirrorGuestTexture(u32 guest_va);

// SYLPHEED: visits every live resolve target (frame dumps).
void ForEachResolveTarget(
    const std::function<void(u32 guest_va, GuestTexture *tex)> &fn);

// SYLPHEED: the resolve target already backing a guest texture, or nullptr.
GuestTexture *FindResolveTarget(u32 guest_va);

// SYLPHEED: the host texture D3DDevice_Resolve writes for a guest texture, created
// (or recreated) at the fetch constant's size in 'format'.
GuestTexture *GetOrCreateResolveTarget(u32 guest_va,
                                       plume::RenderFormat format);

// SYLPHEED: forgets everything mirrored for a destroyed guest texture
// (D3DResource_Destroy): its texel mirror, its resolve target and its cached
// signature, so a texture later created at the same VA is built fresh rather
// than matched to the old one. Returns whether anything was mirrored.
bool EvictGuestTexture(u32 guest_va);

// SYLPHEED: EvictGuestTexture for every mirrored texture whose base or mip address
// lies in [begin, end), a freed allocation. SYLPHEED's fetch constants hold virtual
// addresses, so the range is one too. Returns the count.
u32 EvictGuestTexturesInRange(u32 begin, u32 end);

// Queues the host GuestTexture for deferred teardown.
void EvictNativeTexture(u32 guest_va);

// Free mirrors evicted while frame slot 'slot' was recording. Drained
// post-fence so the descriptor write + free cannot race an in-flight command
// list.
void DrainEvictedNativeTextures(u32 slot);

} // namespace sylpheed::gpu
