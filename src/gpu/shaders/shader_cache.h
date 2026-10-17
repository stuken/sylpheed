// sylpheed - ReXGlue Recompiled Project
//
// Compiled shader cache symbols emitted by XenosRecomp into
// generated/shader_cache.cpp. The generated cpp names these unqualified, so
// they stay in the global namespace with the emitter's exact field order and
// types. Entries are sorted by hash, the XXH3 of the whole shader container.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sylpheed::gpu {
struct GuestShader;
}

struct ShaderCacheEntry {
  const uint64_t hash;
  const uint32_t dxilOffset;
  const uint32_t dxilSize;
  const uint32_t spirvOffset;
  const uint32_t spirvSize;
  const uint32_t specConstantsMask;
  // Filled in at runtime by the renderer when the guest creates the shader.
  sylpheed::gpu::GuestShader *guestShader;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
