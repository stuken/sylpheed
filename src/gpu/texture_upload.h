/**
 * @file    gpu/texture_upload.h
 * @brief   Host mirror textures built from already-untiled guest data, and the
 *          mapped-memory upload path.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>

#include <rex/types.h>

#include "gpu/resources.h"

namespace sylpheed::gpu {

// Copy a texture's mappedMemory scratch into subresource 0 through the upload
// ring and copyTextureRegion.
void UploadTextureFromMapped(GuestTexture *tex);

// One already-untiled, 256-row-aligned mip level.
struct BCMipLevel {
  const void *data;
  size_t size;
  u32 width;
  u32 height;
  u32 row_width_texels; // PlacedFootprint rowWidth for this level
};

// Every builder below takes data that is already untiled with 256-byte-aligned
// rows, and returns a heap-new GuestTexture the caller owns, or nullptr.
GuestTexture *BuildBCMirrorTexture(u32 width, u32 height, u32 format,
                                   const void *block_data,
                                   size_t block_data_size,
                                   u32 staging_row_bytes, u32 row_width_texels,
                                   const plume::RenderComponentMapping *mapping =
                                       nullptr);

// levels[0] is the base.
GuestTexture *BuildBCMirrorTexture2DMips(
    u32 width, u32 height, u32 format, const BCMipLevel *levels,
    u32 level_count, const plume::RenderComponentMapping *mapping = nullptr);

// The same in any host format, for textures that do not come from a guest
// format (texture replacement).
GuestTexture *BuildHostTexture2DMips(
    plume::RenderFormat format, u32 width, u32 height, const BCMipLevel *levels,
    u32 level_count, const plume::RenderComponentMapping *mapping = nullptr);

// Face rows padded to staging_row_bytes. arraySize=6 CUBE.
GuestTexture *BuildBCMirrorTextureCube(u32 width, u32 height, u32 format,
                                       const u8 *faces[6],
                                       size_t face_byte_size,
                                       u32 staging_row_bytes,
                                       u32 row_width_texels,
                                       const plume::RenderComponentMapping *mapping = nullptr);

// Slices contiguous in slice_data, slice 0 first, uploaded in one copy.
GuestTexture *BuildBCMirrorTextureVolume(u32 width, u32 height, u32 depth,
                                         u32 format, const void *slice_data,
                                         size_t slice_data_size,
                                         u32 staging_row_bytes,
                                         u32 row_width_texels);

} // namespace sylpheed::gpu
