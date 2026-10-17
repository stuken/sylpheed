/**
 * @file    gpu/d3d.h
 * @brief   Guest layouts of the XDK 2.0.3215 D3D9 objects Project Sylpheed's
 *          code reads and writes.
 *
 * Ported from reblue (Copyright (c) 2026 Tom Clay, BSD 3-Clause; see
 * licenses/) by way of rr6. The field names are rr6's so the renderer reads
 * them unchanged; every offset here is Sylpheed's own, recovered from its D3D
 * code (config/d3d.toml). The resource headers are the 24-byte layout reblue
 * documents for Blue Dragon, which Sylpheed's XDK shares. The D3DDevice block
 * is laid out like Blue Dragon's up to the constants and like rr6's register
 * shadows (at rr6 - 0x4C0) after them, with its own binding block.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace sylpheed::gpu {

// D3DResource::Common:
//   bits 0-3    header kind (D3DCommonType), what D3DResource_Destroy
//               (0x824C1660) and D3DResource_GetType (0x824C15D0) switch on
//   bit 20      set on every header the creators build
//   bit 22      created with usage 0x200 (D3DDevice_CreateVertexBuffer)
//   bit 30      a surface that is a texture level (D3DSurface_GetDesc,
//               0x824BE760, then reads its parent at +0x18)
//   bit 31      INDEX32 on an index buffer (D3DDevice_DrawIndexedVertices,
//               0x824BF330). On a surface, EDRAM that CreateSurface allocated
//               and Destroy returns.
constexpr u32 kCommonTypeShift = 0;
constexpr u32 kCommonTypeMask = 0x0000000Fu;
constexpr u32 kCommonTextureLevel = 0x40000000u;
constexpr u32 kCommonIndex32 = 0x80000000u;
constexpr u32 kCommonEdramAllocated = 0x80000000u;

// Common bits 0-3. The creators write 1 (D3DDevice_CreateVertexBuffer,
// 0x824C1CD0) and 2 (D3DDevice_CreateIndexBuffer, 0x824C1E08); GetType reads
// 3 and 4 as textures and surfaces.
enum class D3DCommonType : u32 {
  kVertexBuffer = 1,
  kIndexBuffer = 2,
  kTexture = 3, // 2D, cube, volume or array: the fetch constant says which
  kSurface = 4,
};

inline D3DCommonType CommonType(u32 common) {
  return D3DCommonType((common & kCommonTypeMask) >> kCommonTypeShift);
}

// 24-byte header shared by every D3D9 resource.
struct D3DResource {
  be_u32 Common;         // +0x00  the kCommon* bits above
  be_u32 ReferenceCount; // +0x04  1 at create
  // +0x08  GPU fence of the last use: unbinding stores the device's current
  // fence here (D3DDevice_SetStreamSource, 0x824B3F40).
  be_u32 Fence;
  be_u32 ReadFence;  // +0x0C
  be_u32 Identifier; // +0x10
  be_u32 BaseFlush;  // +0x14  0xFFFF0000 at create
};
static_assert(sizeof(D3DResource) == 24);

// The pViewport argument to D3DDevice_SetViewport (0x824B3D00), and the
// layout of the device's cached viewport at D3DDevice+0x3068.
struct D3DViewport9 {
  be_u32 X;
  be_u32 Y;
  be_u32 Width;
  be_u32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT (LTRB), the device's scissor rect at D3DDevice+0x3080.
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT: the 6-dword Xenos texture descriptor that
// textures carry at +0x1C. native_texture_mirror decodes it.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

// D3DTexture / D3DCubeTexture / D3DVolumeTexture / D3DArrayTexture, 52 bytes.
// D3DDevice_SetTexture (0x824BE880) copies Format into the device's fetch
// slot for the sampler.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18  0xFFFF0000 at create
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

// D3DSurface, 48 bytes (D3DDevice_CreateSurface, 0x824BE638, allocates 0x30).
// D3DSurface_GetDesc (0x824BE760) reads the format from +0x28, the size from
// +0x24 and MultiSampleType from SurfaceInfo bits 0-1. A texture-level
// surface (kCommonTextureLevel) names its texture at +0x18 and its level in
// +0x1C bits 28-31 instead.
struct D3DSurface {
  D3DResource resource; // +0x00
  // +0x18  RB_SURFACE_INFO: EDRAM pitch, MSAA samples in bits 16-17 (the
  // halfword at +0x18 is what GetDesc masks). Parent texture on a level.
  be_u32 SurfaceInfo;
  // +0x1C  RB_COLOR_INFO / RB_DEPTH_INFO: EDRAM base tile in bits 0-11
  // (D3DResource_Destroy hands it back), level in bits 28-31 on a level.
  be_u32 EdramInfo;
  be_u32 HiZInfo;  // +0x20
  be_u32 SizeBits; // +0x24  (width-1) << 18 | (height-1) << 3
  be_u32 Format;   // +0x28  guest D3DFORMAT
  be_u32 Size;     // +0x2C  bytes: EDRAM tiles * 5120 for a created surface
};
static_assert(sizeof(D3DSurface) == 48);

// Written by D3DSurface_GetDesc (0x824BE760) and the texture level desc
// (0x824BD4B0).
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08  always 0
  be_u32 Pool;               // +0x0C  always 0
  be_u32 MultiSampleType;    // +0x10  SurfaceInfo bits 0-1 of the halfword
  be_u32 MultiSampleQuality; // +0x14  always 0
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// D3DVertexBuffer, 32 bytes. Fetch0/Fetch1 are the first two words of the
// buffer's Xenos vertex fetch constant: the data's VA | 3 (the vertex fetch
// type), then its size in bytes (bits 2-25) | 2 (the endian swap)
// (D3DDevice_CreateVertexBuffer, 0x824C1CD0).
struct D3DVertexBuffer {
  D3DResource resource; // +0x00
  be_u32 Fetch0;        // +0x18
  be_u32 Fetch1;        // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);
static_assert(offsetof(D3DVertexBuffer, Fetch0) == 0x18);

// D3DIndexBuffer, 32 bytes: the data's VA and size, both raw
// (D3DDevice_CreateIndexBuffer, 0x824C1E08). Common's kCommonIndex32 marks
// 32-bit indices.
struct D3DIndexBuffer {
  D3DResource resource; // +0x00
  be_u32 Address;       // +0x18
  be_u32 Size;          // +0x1C
};
static_assert(sizeof(D3DIndexBuffer) == 32);
static_assert(offsetof(D3DIndexBuffer, Address) == 0x18);

// A shader is a D3DResource-style header (Common type 7) followed by a copy of
// its container: 0x38 bytes for a pixel shader (D3DDevice_CreatePixelShader,
// 0x824BA510), 0x250 for a vertex shader (D3DDevice_CreateVertexShader,
// 0x824BA600). The renderer tracks both by the address the creator returns
// and reads neither header.

// The microcode blob CreateVertexShader/CreatePixelShader are handed, layout
// mirrors thirdparty/XenosRecomp/XenosRecomp/shader.h.
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// Direct3D_CreateDevice (0x824C2AF0) allocates this through D3D_AllocDevice.
constexpr u32 kD3DDeviceAllocSize = 0x5000;

// dword[0] bits [1:0] of a fetch constant tag what it describes.
constexpr u32 kFetchTypeMask = 0x3;
constexpr u32 kFetchTypeTexture = 2;
constexpr u32 kFetchTypeVertex = 3;

// 24-byte Xenos GPU fetch descriptor. D3DDevice holds 32 in fetchConstants[].
struct DeviceFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(DeviceFetchConstant) == 24);

// Render and sampler state dispatch, {Get, Set, Default} per entry.
// D3D__InitializeStateTables (0x824C7508) copies Set/Get into the device and
// calls Set(Default).
constexpr u32 kRenderStateTableGuestAddr = 0x82870DC8;
constexpr u32 kRenderStateTableEntries = 96;
constexpr u32 kSamplerStateTableGuestAddr = 0x82871248;
constexpr u32 kSamplerStateTableEntries = 20;
constexpr u32 kSamplerCount = 26;
constexpr u32 kStreamCount = 16;

// Shared guest memory. Only fields with a citation in config/d3d.toml are
// named. Everything else is padding until a function proves its meaning.
struct D3DDevice {
  be_u64 m_Mask[5];        // +0x000  dirty masks the Set* functions OR into
  be_u32 m_pRing;          // +0x028  D3D__MakeSpace (0x824C0B00) returns it
  be_u32 m_pRingEnd;       // +0x02C  hard end of the current segment
  be_u32 m_pRingLimit;     // +0x030  callers compare m_pRing against it
  be_u32 m_ReferenceCount; // +0x034  D3DDevice_Release (0x824C2920)
  be_u32 m_SetRenderStateCall[kRenderStateTableEntries];   // +0x038
  be_u32 m_SetSamplerStateCall[kSamplerStateTableEntries]; // +0x1B8
  be_u32 m_GetRenderStateCall[kRenderStateTableEntries];   // +0x208
  be_u32 m_GetSamplerStateCall[kSamplerStateTableEntries]; // +0x388
  u8 pad_3D8[0x400 - 0x3D8];
  // D3DDevice_SetTexture (0x824BE880) fills slot s. Vertex fetch constants are
  // 8 bytes, three to a slot: stream i lives at +0x6F8 - 8 * i
  // (D3DDevice_SetStreamSource, 0x824B3F40).
  DeviceFetchConstant fetchConstants[32]; // +0x400
  be_f32 vsFloatConstants[256][4];        // +0x700  0x824B8830
  be_f32 psFloatConstants[256][4];        // +0x1700 0x824B8940
  // Bool constants b0..b255, one bit each (bit n % 32 of word n / 32): VS
  // b0..b127 from 0x824B8A50, PS b128..b255 from 0x824B8AA8 (its b0 lands on
  // word 4). Shader BOOL_BIT(n) reads the same layout.
  be_u32 boolConstants[8];                // +0x2700
  be_u32 vsIntConstants[16];              // +0x2720 0x824B8B00
  be_u32 psIntConstants[16];              // +0x2760 0x824B8B50
  // Flushed to PA_CL_UCP_0_X (0x2388) by 0x824C4608 before a draw.
  be_f32 clipPlanes[6][4]; // +0x27A0
  u8 pad_2800[0x2844 - 0x2800];
  // Xenos register shadows the render-state setters write, one dword per
  // register: the 0x21xx group from +0x284C, the 0x22xx group from +0x28B4.
  // PA_SC_WINDOW_SCISSOR_TL/BR (0x2081/0x2082): x in bits 0-14, y in 16-30,
  // BR exclusive (D3DDevice_SetScissorRect, 0x824B3C38).
  be_u32 paScWindowScissorTL; // +0x2844
  be_u32 paScWindowScissorBR; // +0x2848
  u8 pad_284C[0x285C - 0x284C];
  be_u32 rbColorMask;            // +0x285C  RB_COLOR_MASK (0x2104)
  u8 pad_2860[0x287C - 0x2860];
  be_u32 rbStencilRefMaskBF;     // +0x287C  RB_STENCILREFMASK_BF (0x210C)
  be_u32 rbStencilRefMask;       // +0x2880  RB_STENCILREFMASK (0x210D)
  be_f32 rbAlphaRef;             // +0x2884  RB_ALPHA_REF (0x210E), 0..1
  be_f32 viewportScaleOffset[6]; // +0x2888  D3DDevice_SetViewport
  u8 pad_28A0[0x28B4 - 0x28A0];
  be_u32 rbDepthControl;      // +0x28B4  RB_DEPTHCONTROL (0x2200)
  be_u32 rbBlendControl0;     // +0x28B8  RB_BLENDCONTROL0 (0x2201)
  be_u32 rbColorControl;      // +0x28BC  RB_COLORCONTROL (0x2202)
  u8 pad_28C0[0x28C4 - 0x28C0];
  // PA_CL_CLIP_CNTL (0x2204): bits 0-5 enable user clip planes 0-5
  // (D3DRS_CLIPPLANEENABLE, 0x824B2480).
  be_u32 paClClipCntl;        // +0x28C4
  be_u32 paSuScModeCntl;      // +0x28C8  PA_SU_SC_MODE_CNTL (0x2205)
  // PA_CL_VTE_CNTL (0x2206), written by D3DRS_VIEWPORTENABLE (0x824B2C18).
  be_u32 paClVteCntl;         // +0x28CC
  u8 pad_28D0[0x28D8 - 0x28D0];
  be_u32 rbBlendControl1[3];  // +0x28D8  RB_BLENDCONTROL1..3 (0x2209..)
  u8 pad_28E4[0x2940 - 0x28E4];
  // Bit 0 is PA_SU_VTX_CNTL.PIX_CENTER, written by D3DRS_HALFPIXELOFFSET
  // (0x824B2FD0).
  be_u32 paSuVtxCntl;           // +0x2940
  u8 pad_2944[0x29D0 - 0x2944];
  // PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET}, from
  // D3DRS_SLOPESCALEDEPTHBIAS (0x824B24D8) and D3DRS_DEPTHBIAS (0x824B25A0).
  be_f32 polyOffsetFrontScale;  // +0x29D0
  be_f32 polyOffsetFrontOffset; // +0x29D4
  be_f32 polyOffsetBackScale;   // +0x29D8
  be_f32 polyOffsetBackOffset;  // +0x29DC
  u8 pad_29E0[0x2D20 - 0x29E0];
  // D3DDevice_SetVertexDeclaration (0x824B8FC8) and D3DDevice_SetFVF
  // (0x824BA6D0, which binds the declaration it builds at +0x2EA0).
  be_u32 vertexDeclaration;     // +0x2D20
  u8 pad_2D24[0x2D48 - 0x2D24];
  // Blend as set through D3DRS (SRCBLEND..BLENDOPALPHA), applied to the four
  // RB_BLENDCONTROLs only while ALPHABLENDENABLE (blendFlags bit 31) is on.
  // Bit 30 is SEPARATEALPHABLENDENABLE.
  be_u32 blendStaged; // +0x2D48
  be_u32 blendFlags;  // +0x2D4C
  u8 pad_2D50[0x2F90 - 0x2D50];
  be_u32 fvf;                        // +0x2F90  D3DDevice_SetFVF
  be_u32 indexBuffer;               // +0x2F94  D3DDevice_SetIndices 0x824B40F0
  be_u32 renderTargets[4];           // +0x2F98  D3DDevice_SetRenderTarget
  be_u32 depthStencil;               // +0x2FA8  D3DDevice_SetDepthStencilSurface
  // Vertex buffer per stream. The stream offset is not kept: SetStreamSource
  // folds it into the stream's fetch constant (see StreamOffset).
  be_u32 streamBuffers[kStreamCount]; // +0x2FAC
  u8 pad_2FEC[0x2FF0 - 0x2FEC];
  u8 streamStrides[kStreamCount];     // +0x2FF0  stride / 4
  be_u32 textures[kSamplerCount];     // +0x3000  D3DDevice_SetTexture
  D3DViewport9 viewport;      // +0x3068  D3DDevice_SetViewport
  D3DRect scissorRect;        // +0x3080  D3DDevice_SetScissorRect
  // SetPixelShader (0x824B8BF0) reads its container at +0x38, the pixel
  // shader header's size; the SetVertexShader body (0x824B8E28) at +0x250.
  be_u32 pixelShader;         // +0x3090  D3DDevice_SetPixelShader
  be_u32 vertexShader;        // +0x3094  D3DDevice_SetVertexShader
  u8 pad_3098[0x4FD8 - 0x3098];
  be_u32 behaviorFlags;       // +0x4FD8  Direct3D_CreateDevice
  u8 pad_4FDC[0x5000 - 0x4FDC];
};
static_assert(sizeof(D3DDevice) == kD3DDeviceAllocSize);
static_assert(offsetof(D3DDevice, m_pRing) == 0x028);
static_assert(offsetof(D3DDevice, m_ReferenceCount) == 0x034);
static_assert(offsetof(D3DDevice, m_SetRenderStateCall) == 0x038);
static_assert(offsetof(D3DDevice, m_SetSamplerStateCall) == 0x1B8);
static_assert(offsetof(D3DDevice, m_GetRenderStateCall) == 0x208);
static_assert(offsetof(D3DDevice, m_GetSamplerStateCall) == 0x388);
static_assert(offsetof(D3DDevice, fetchConstants) == 0x400);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x700);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1700);
static_assert(offsetof(D3DDevice, boolConstants) == 0x2700);
static_assert(offsetof(D3DDevice, psIntConstants) == 0x2760);
static_assert(offsetof(D3DDevice, clipPlanes) == 0x27A0);
static_assert(offsetof(D3DDevice, paScWindowScissorTL) == 0x2844);
static_assert(offsetof(D3DDevice, rbColorMask) == 0x285C);
static_assert(offsetof(D3DDevice, rbStencilRefMask) == 0x2880);
static_assert(offsetof(D3DDevice, rbAlphaRef) == 0x2884);
static_assert(offsetof(D3DDevice, viewportScaleOffset) == 0x2888);
static_assert(offsetof(D3DDevice, rbDepthControl) == 0x28B4);
static_assert(offsetof(D3DDevice, rbColorControl) == 0x28BC);
static_assert(offsetof(D3DDevice, paClClipCntl) == 0x28C4);
static_assert(offsetof(D3DDevice, paSuScModeCntl) == 0x28C8);
static_assert(offsetof(D3DDevice, paClVteCntl) == 0x28CC);
static_assert(offsetof(D3DDevice, rbBlendControl1) == 0x28D8);
static_assert(offsetof(D3DDevice, paSuVtxCntl) == 0x2940);
static_assert(offsetof(D3DDevice, polyOffsetFrontScale) == 0x29D0);
static_assert(offsetof(D3DDevice, blendFlags) == 0x2D4C);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2D20);
static_assert(offsetof(D3DDevice, fvf) == 0x2F90);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x2F94);
static_assert(offsetof(D3DDevice, renderTargets) == 0x2F98);
static_assert(offsetof(D3DDevice, depthStencil) == 0x2FA8);
static_assert(offsetof(D3DDevice, streamBuffers) == 0x2FAC);
static_assert(offsetof(D3DDevice, streamStrides) == 0x2FF0);
static_assert(offsetof(D3DDevice, textures) == 0x3000);
static_assert(offsetof(D3DDevice, viewport) == 0x3068);
static_assert(offsetof(D3DDevice, scissorRect) == 0x3080);
static_assert(offsetof(D3DDevice, pixelShader) == 0x3090);
static_assert(offsetof(D3DDevice, vertexShader) == 0x3094);
static_assert(offsetof(D3DDevice, behaviorFlags) == 0x4FD8);

// The GPU address D3D writes into fetch constants for a CPU virtual address
// (D3DDevice_SetStreamSource and SetTexture): the low 29 bits, plus 0x1000 for
// the 0xE0000000 view whose physical pages start 4 KB in.
inline u32 GpuAddress(u32 va) {
  return (va & 0x1FFFFFFFu) + (((va >> 20) + 0x200u) & 0x1000u);
}

// The byte offset SetStreamSource was given for stream i: the stream's fetch
// base less the vertex buffer's own (D3DDevice_GetStreamSource, 0x824B4068,
// computes it the same way).
inline u32 StreamOffset(const D3DDevice &dev, u32 stream,
                        const D3DVertexBuffer &vb) {
  const auto *words = reinterpret_cast<const be_u32 *>(&dev.fetchConstants[0]);
  const u32 fetch0 = words[(0x2F8u - 8u * stream) / 4u];
  return (u32(fetch0) & ~0x3u) - GpuAddress(u32(vb.Fetch0) & ~0x3u);
}

constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

} // namespace sylpheed::gpu
