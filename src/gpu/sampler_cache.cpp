/**
 * @file    gpu/sampler_cache.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/sampler_cache.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "gpu/bindless_allocator.h"
#include "gpu/device.h"
#include "gpu/settings.h"

namespace sylpheed::gpu {

namespace {

namespace xe = rex::graphics::xenos;

// Only these fields ever vary, so they form the key, not the full desc.
// anisoEnabled is separate: a game-set 1:1 limit and off both carry maxAniso 1.
struct DescKey {
  plume::RenderTextureAddressMode u;
  plume::RenderTextureAddressMode v;
  plume::RenderTextureAddressMode w;
  plume::RenderFilter min;
  plume::RenderFilter mag;
  plume::RenderMipmapMode mip;
  plume::RenderBorderColor border;
  u32 maxAniso;
  bool anisoEnabled;
  float minLOD;
  float maxLOD;
  float lodBias;

  bool operator==(const DescKey &other) const noexcept {
    return u == other.u && v == other.v && w == other.w && min == other.min &&
           mag == other.mag && mip == other.mip && border == other.border &&
           maxAniso == other.maxAniso && anisoEnabled == other.anisoEnabled &&
           minLOD == other.minLOD && maxLOD == other.maxLOD &&
           lodBias == other.lodBias;
  }
};

struct DescKeyHash {
  size_t operator()(const DescKey &k) const noexcept {
    u64 bits = 0;
    bits |= static_cast<u64>(k.u);
    bits |= static_cast<u64>(k.v) << 4;
    bits |= static_cast<u64>(k.w) << 8;
    bits |= static_cast<u64>(k.min) << 12;
    bits |= static_cast<u64>(k.mag) << 16;
    bits |= static_cast<u64>(k.mip) << 20;
    bits |= static_cast<u64>(k.border) << 24;
    bits |= static_cast<u64>(k.maxAniso) << 28;
    bits |= static_cast<u64>(k.anisoEnabled) << 36;
    // minLOD is a whole mip level (0-15). maxLOD is FLT_MAX or minLOD plus 0
    // or 0.25, so only that offset's presence needs hashing.
    bits |= static_cast<u64>(k.minLOD) << 40;
    bits |= static_cast<u64>(k.maxLOD != k.minLOD) << 44;
    // lodBias is a 10-bit fetch field over 32.
    bits |= static_cast<u64>(static_cast<i64>(k.lodBias * 32.0f) & 0x3FF) << 45;
    return std::hash<u64>{}(bits);
  }
};

struct CachedSampler {
  std::unique_ptr<plume::RenderSampler> sampler;
  u32 slot = 0;
};

struct Cache {
  std::unordered_map<DescKey, CachedSampler, DescKeyHash> map;
  std::mutex mutex;
};

Cache &cache() {
  static Cache c;
  return c;
}

// plume has no half-way clamp, so those take the plain clamp of the same
// mirroring.
plume::RenderTextureAddressMode ConvertClamp(xe::ClampMode mode) {
  using A = plume::RenderTextureAddressMode;
  switch (mode) {
  case xe::ClampMode::kRepeat:
    return A::WRAP;
  case xe::ClampMode::kMirroredRepeat:
    return A::MIRROR;
  case xe::ClampMode::kClampToEdge:
  case xe::ClampMode::kClampToHalfway:
    return A::CLAMP;
  case xe::ClampMode::kMirrorClampToEdge:
  case xe::ClampMode::kMirrorClampToHalfway:
    return A::MIRROR_ONCE;
  case xe::ClampMode::kClampToBorder:
    return A::BORDER;
  case xe::ClampMode::kMirrorClampToBorder:
    return A::MIRROR_ONCE;
  }
  return A::CLAMP;
}

// Only kPoint is nearest: kLinear and kUseFetchConst sample linearly. For the
// mip filter, kBaseMap is handled before this is asked.
bool IsNearest(xe::TextureFilter filter) {
  return filter == xe::TextureFilter::kPoint;
}

// The fetch constant's anisotropy limit, 0 when off. The shaders' tfetch
// instructions all defer to the fetch constant (kUseFetchConst), so it is the
// game's whole request.
u32 GuestAnisotropy(xe::AnisoFilter filter) {
  switch (filter) {
  case xe::AnisoFilter::kMax_1_1:
    return 1;
  case xe::AnisoFilter::kMax_2_1:
    return 2;
  case xe::AnisoFilter::kMax_4_1:
    return 4;
  case xe::AnisoFilter::kMax_8_1:
    return 8;
  case xe::AnisoFilter::kMax_16_1:
    return 16;
  default:
    return 0;
  }
}

} // namespace

plume::RenderSamplerDesc DecodeFromFetch(const u32 fc[6]) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));

  plume::RenderSamplerDesc d;
  d.addressU = ConvertClamp(fetch.clamp_x);
  d.addressV = ConvertClamp(fetch.clamp_y);
  d.addressW = ConvertClamp(fetch.clamp_z);

  d.magFilter = IsNearest(fetch.mag_filter) ? plume::RenderFilter::NEAREST
                                            : plume::RenderFilter::LINEAR;
  d.minFilter = IsNearest(fetch.min_filter) ? plume::RenderFilter::NEAREST
                                            : plume::RenderFilter::LINEAR;
  // kBaseMap (D3DTEXF_NONE, the device default, which games keep for many
  // textures, mipped ones included) samples just the most
  // detailed allowed level, mip_min_level, as the console did. As in the SDK's texture caches: point mip
  // mode with MaxLOD a quarter level above MinLOD keeps the
  // magnification/minification decision (Vulkan's GL_NEAREST/GL_LINEAR
  // emulation), and with anisotropy, which D3D12 always filters linearly
  // between levels, MaxLOD equals MinLOD.
  const bool base_map = fetch.mip_filter == xe::TextureFilter::kBaseMap;
  d.mipmapMode = base_map || IsNearest(fetch.mip_filter)
                     ? plume::RenderMipmapMode::NEAREST
                     : plume::RenderMipmapMode::LINEAR;
  d.minLOD = static_cast<float>(fetch.mip_min_level);
  // Signed, 5 fractional bits. The shaders' tfetch instructions carry no bias of their
  // own, so the sampler can apply it whole.
  d.mipLODBias = static_cast<float>(fetch.lod_bias) / 32.0f;

  // sylpheed_anisotropy "default" (0) takes the fetch constant's level; 2 to 16
  // forces that level. Either way only when the
  // filters are already LINEAR: D3D12_FILTER_ANISOTROPIC hardcodes LINEAR
  // min/mag/mip, so enabling aniso on a POINT sampler silently upgrades it to
  // bilinear on D3D12 but not on Vulkan. kBaseMap qualifies, since its LOD
  // clamp leaves no second level to filter towards; its taps along the
  // footprint's long axis still come from the one level.
  const i32 forced = Settings::Get().Anisotropy();
  const u32 aniso =
      forced > 0 ? static_cast<u32>(forced) : GuestAnisotropy(fetch.aniso_filter);
  const bool aniso_on = aniso > 0 &&
                        (base_map ||
                         d.mipmapMode == plume::RenderMipmapMode::LINEAR) &&
                        d.minFilter == plume::RenderFilter::LINEAR &&
                        d.magFilter == plume::RenderFilter::LINEAR;
  d.anisotropyEnabled = aniso_on;
  d.maxAnisotropy = aniso_on ? aniso : 1u;
  if (base_map)
    d.maxLOD = d.minLOD + (aniso_on ? 0.0f : 0.25f);

  d.borderColor = fetch.border_color == xe::BorderColor::k_ABGR_White
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  return d;
}

u32 ResolveSlotLocked(const plume::RenderSamplerDesc &desc) {
  auto &s = state();
  if (!s.ready || !s.device || !s.sampler_descriptor_set)
    return 0;

  const DescKey key{
      desc.addressU,    desc.addressV,      desc.addressW,
      desc.minFilter,   desc.magFilter,     desc.mipmapMode,
      desc.borderColor, desc.maxAnisotropy, desc.anisotropyEnabled,
      desc.minLOD,      desc.maxLOD,        desc.mipLODBias,
  };

  auto &c = cache();
  {
    std::lock_guard lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end())
      return it->second.slot;
  }

  // Heap slot alloc relies on the state().mutex the caller already holds.
  const u32 slot = BindlessAllocateSlot(s.sampler_descriptor_used, 1, 0);
  if (slot == 0) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 4) {
      SYLPHEED_WARN(
          "[sampler-cache] bindless sampler heap full, falling back to slot 0");
    }
    return 0;
  }

  auto sampler = s.device->createSampler(desc);
  s.sampler_descriptor_set->setSampler(slot, sampler.get());

  std::lock_guard lock(c.mutex);
  // On a race, return the winner's slot. Ours leaks (no reclaim on drop here).
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.slot;
  c.map.emplace(key, CachedSampler{std::move(sampler), slot});
  return slot;
}

} // namespace sylpheed::gpu
