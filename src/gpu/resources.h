/**
 * @file    gpu/resources.h
 * @brief   The records the renderer keeps for each guest D3D9 resource:
 *          textures, buffers, shaders, vertex declarations.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * SYLPHEED's own D3D creates every guest resource, so these are host-side records
 * keyed by the guest header's address. No guest code reads them.
 */
#pragma once

#include <memory>
#include <rex/types.h>
#include <unordered_map>
#include <unordered_set>

#include <plume_render_interface.h>

#include "gpu/d3d.h"
#include "gpu/shaders/shader_cache.h"

namespace sylpheed::gpu {

enum class ResourceType : u32 {
  Texture = 0,
  VolumeTexture = 1,
  VertexBuffer = 2,
  IndexBuffer = 3,
  RenderTarget = 4,
  DepthStencil = 5,
  VertexDeclaration = 6,
  VertexShader = 7,
  PixelShader = 8,
};

struct GuestTexture {
  ResourceType type = ResourceType::Texture;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderTexture> textureHolder;
  plume::RenderTexture *texture = nullptr;
  std::unique_ptr<plume::RenderTextureView> textureView;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 0;
  u32 mipLevels = 1;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 guestFormat = 0;
  u32 descriptorIndex = ~u32{0};
  // Fetch result components whose TextureSign is kGamma (bit i: component i,
  // after the fetch constant's swizzle). The texture holds the encoded texels;
  // the shader's fetch applies the console's piecewise-linear degamma, which
  // the host's sRGB formats only approximate. Published in the top bits of the
  // descriptor index (shader_common.h textureGamma).
  u8 gammaMask = 0;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::UNKNOWN;
  plume::RenderSampleCounts sampleCount = plume::RenderSampleCount::COUNT_1;
  GuestTexture *sourceSurface = nullptr;
  std::unordered_set<GuestTexture *> destinationTextures;
  float resolveScale = 1.0f;
  // Host texels per guest texel: Output::SurfaceScale for surfaces and
  // resolve targets, the resolution scale for the back buffer, 1 for mirrors.
  u32 hostScale = 1;
  // A resolve target kept below the resolution scale (Output::SurfaceScale).
  // Higher-scale passes sample it through a smooth upscale.
  bool reducedScale = false;
  // This texture box-filtered down to a lower-scale target's resolution, for
  // draws into that target (BuildGuestScaleCopiesLocked).
  std::unique_ptr<GuestTexture> guestScaleCopy;
  // What guestScaleCopy was last filtered from: the sampled texture and its
  // contentVersion then. A pass binding the same unchanged source reuses it.
  const GuestTexture *guestScaleCopyFrom = nullptr;
  u64 guestScaleCopyVersion = 0;
  // Bumped whenever the texture may have been written: bound as a draw or
  // clear target, copied or resolved into, resized. Never decreases.
  u64 contentVersion = 0;
  // This texture FXAA'd, for the colour grading pass (BuildFxaaCopyLocked).
  std::unique_ptr<GuestTexture> fxaaCopy;
  u32 resolveLevel = 0;
  u32 resolveFace = 0;
  bool registered = false;
  // A surface-registry surface some guest surface maps to. Cleared when that
  // guest surface is destroyed, so the registry can reissue it.
  bool held = false;
  bool pendingDestroy = false;
  bool pendingGPURead = false;
  bool reflection = false;
  bool framebufferAttached = false;
  std::unordered_map<const plume::RenderTexture *,
                     std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers;
  u32 mappedMemory = 0;
  std::unique_ptr<GuestTexture> companion2D;

  GuestTexture() = default;
  explicit GuestTexture(ResourceType t) : type(t) {}
  GuestTexture(const GuestTexture &) = delete;
  GuestTexture &operator=(const GuestTexture &) = delete;
};

struct GuestBuffer {
  ResourceType type = ResourceType::VertexBuffer;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderBuffer> buffer;
  // The guest data the mirror copies from (guest_buffers.cpp). ownsMirror is
  // set only when the renderer allocated it, so the graveyard frees it.
  u32 guestMirrorVa = 0;
  bool ownsMirror = false;
  u32 dataSize = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 guestFormat = 0;

  bool hasBuffer() const { return buffer != nullptr; }
  // 'offset' bytes in. Caller checks hasBuffer().
  plume::RenderBufferReference bufferRef(u32 offset) const {
    return buffer->at(offset);
  }

  GuestBuffer() = default;
  explicit GuestBuffer(ResourceType t) : type(t) {}
  GuestBuffer(const GuestBuffer &) = delete;
  GuestBuffer &operator=(const GuestBuffer &) = delete;
};

// VertexShader / PixelShader.
struct GuestShader {
  ResourceType type = ResourceType::VertexShader;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderShader> shader;
  const ShaderCacheEntry *shaderCacheEntry = nullptr;
  // Spec constant shaders link a distinct RenderShader per masked
  // g_SpecConstants() value. 'shader' above is the mask-0 variant.
  std::unordered_map<u32, std::unique_ptr<plume::RenderShader>> linkedShaders;

  GuestShader() = default;
  explicit GuestShader(ResourceType t) : type(t) {}
  GuestShader(const GuestShader &) = delete;
  GuestShader &operator=(const GuestShader &) = delete;
};

struct GuestVertexElement {
  be_u16 stream;
  be_u16 offset;
  be_u32 type;
  u8 method;
  u8 usage;
  u8 usageIndex;
  u8 padding;
};
static_assert(sizeof(GuestVertexElement) == 12);

struct GuestVertexDeclaration {
  ResourceType type = ResourceType::VertexDeclaration;
  u32 selfVa = 0;

  u64 hash = 0;
  std::unique_ptr<plume::RenderInputElement[]> inputElements;
  std::unique_ptr<GuestVertexElement[]> vertexElements;
  u32 inputElementCount = 0;
  u32 vertexElementCount = 0;
  // Per-semantic-index bitmask: bit N = the element with semantic index N uses
  // a 16-bit packed format (SHORT2/4, *_N, USHORT2/4N, FLOAT16_2/4) and needs
  // the shader-side .yxwz swap (swapFloats()) to undo the engine's bswap32 of
  // physical VBs. Pushed into g_SwappedXxx shared constants.
  u32 swappedTexcoords = 0;
  u32 swappedNormals = 0;
  u32 swappedBinormals = 0;
  u32 swappedTangents = 0;
  u32 swappedBlendWeights = 0;
  u32 swappedPositions = 0;
  // Per-TEXCOORD bit for SHORT2/SHORT4 (non-normalized) elements. Those IA
  // slots bind R16G16(B16A16)_UINT so sintTexcoord() sign-extends and casts
  // back to the X360 raw-int-as-float vfetch the shader math (e.g. (uv+1)/512)
  // expects. SHORT2N/4N stay SNORM and leave the bit clear.
  u32 sintTexcoords = 0;
  bool hasR11G11B10Normal = false;
  bool vertexStreams[16]{};
  u32 indexVertexStream = 0;

  GuestVertexDeclaration() = default;
  GuestVertexDeclaration(const GuestVertexDeclaration &) = delete;
  GuestVertexDeclaration &operator=(const GuestVertexDeclaration &) = delete;
};

// Reserved bindless descriptors for unbound textures: a shader may legally
// index these for dimensions it does not use.
enum : u32 {
  kNullTexture2DDescriptorIndex = 0,
  kNullTexture3DDescriptorIndex = 1,
  kNullTextureCubeDescriptorIndex = 2,
  kNullTextureDescriptorCount = 3,
};

// Sentinel for "not registered with the bindless heap yet".
constexpr u32 kInvalidDescriptorIndex = ~u32{0};

// 256-aligned (PITCH_ALIGNMENT 0x100) row pitch in bytes for one mip-0 row of
// 'tex'. Drives the LockRect scratch size and the copyTextureRegion footprint.
u32 ComputeTexturePitch(const GuestTexture *tex);

GuestShader *CreateShader(const be_u32 *function, ResourceType type);

// Mask-0 cache shaders return GuestShader::shader directly, and
// spec constant ones are DXC-linked per masked value into linkedShaders.
// nullptr on failure.
plume::RenderShader *GetOrLinkShader(GuestShader *shader, u32 specConstants);

// By microcode hash (== ShaderCacheEntry::hash). nullptr if not yet created.
GuestShader *FindGuestShaderByHash(u64 hash);

// 4x4 neutral-white, process lifetime. SetTexture substitutes it for content
// textures reblue cannot resolve.
GuestTexture *GetOrCreateDebugTexture();

} // namespace sylpheed::gpu
