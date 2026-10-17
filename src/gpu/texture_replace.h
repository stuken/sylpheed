// sylpheed - ReXGlue Recompiled Project
//
// Texture replacement: swaps a guest texture's host mirror for a DDS file of
// any resolution, and dumps textures for editing.
//
// A texture is identified by its content: an XXH3 hash of its untiled top mip
// level, seeded with its guest format and size. Files are named
// <hash>_<width>x<height>_<format>.dds and matched on the leading 16 hex
// digits, so the rest of the name is free to change.
//   sylpheed_texture_dump     writes each 2D texture the game uses, once, with all
//                        its mip levels, to <user data>/textures/dump.
//   sylpheed_texture_replace  loads <user data>/textures/replace (recursively) at
//                        the first texture, and uses a matching file instead
//                        of the game's texture.

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"
#include "gpu/texture_upload.h"

namespace sylpheed::gpu {

// Called with a 2D texture's untiled levels (levels[0] the top mip, rows
// 256-byte aligned) before its mirror is built. Returns the replacement host
// texture, heap-new and owned by the caller, or nullptr to build the game's
// own. Dumps the texture first when sylpheed_texture_dump asks.
//
// guest_format is the Xenos texture format, mapping the component mapping the
// game's mirror would use. A k_8_8_8_8 texture is dumped with its mapping
// applied, so it reads correctly in an image editor, and its replacement is
// used as is. Other formats are dumped as stored, and their replacements take
// the same mapping, so a file authored from a dump keeps its channel layout.
GuestTexture *ReplaceOrDumpTexture(u32 guest_format, u32 width, u32 height,
                                   const BCMipLevel *levels, u32 level_count,
                                   const plume::RenderComponentMapping &mapping);

} // namespace sylpheed::gpu
