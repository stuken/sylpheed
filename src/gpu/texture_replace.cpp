// sylpheed - ReXGlue Recompiled Project
//
// Texture replacement and dumping; see texture_replace.h.

#include "gpu/texture_replace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/runtime.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/settings.h"
#include "gpu/format.h"

REXCVAR_DEFINE_BOOL(sylpheed_texture_dump, false, kCvarGroup,
                    "Write each texture the game uses to "
                    "<user data>/textures/dump as a DDS file, once");
REXCVAR_DEFINE_BOOL(sylpheed_texture_replace, true, kCvarGroup,
                    "Use DDS files from <user data>/textures/replace in place "
                    "of the game's textures they match");

namespace sylpheed::gpu {

namespace {

namespace fs = std::filesystem;
namespace xe = rex::graphics::xenos;

// --- DDS --------------------------------------------------------------------

constexpr u32 FourCC(char a, char b, char c, char d) {
  return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) |
         (u32(u8(d)) << 24);
}

constexpr u32 kDdsMagic = FourCC('D', 'D', 'S', ' ');
constexpr u32 kDdsdCaps = 0x1, kDdsdHeight = 0x2, kDdsdWidth = 0x4,
              kDdsdPixelFormat = 0x1000, kDdsdMipMapCount = 0x20000,
              kDdsdLinearSize = 0x80000, kDdsdPitch = 0x8;
constexpr u32 kDdpfAlphaPixels = 0x1, kDdpfFourCC = 0x4, kDdpfRgb = 0x40,
              kDdpfLuminance = 0x20000;
constexpr u32 kDdsCapsComplex = 0x8, kDdsCapsTexture = 0x1000,
              kDdsCapsMipMap = 0x400000;
constexpr u32 kDdsCaps2Cubemap = 0x200, kDdsCaps2Volume = 0x200000;

struct DdsPixelFormat {
  u32 size, flags, fourcc, rgb_bits, r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
  u32 size, flags, height, width, pitch_or_linear_size, depth, mip_count;
  u32 reserved1[11];
  DdsPixelFormat pf;
  u32 caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
  u32 dxgi_format, resource_dimension, misc_flag, array_size, misc_flags2;
};
static_assert(sizeof(DdsHeader) == 124 && sizeof(DdsHeaderDx10) == 20);

// Bytes in one tightly packed row of blocks, and how many such rows.
u32 PackedRowBytes(plume::RenderFormat fmt, u32 width) {
  const u32 bw = plume::RenderFormatBlockWidth(fmt);
  return (width + bw - 1) / bw * plume::RenderFormatSize(fmt);
}
u32 BlockRows(plume::RenderFormat fmt, u32 height) {
  const u32 bw = plume::RenderFormatBlockWidth(fmt);
  return (height + bw - 1) / bw;
}

const char *FormatName(plume::RenderFormat fmt) {
  switch (fmt) {
  case plume::RenderFormat::BC1_UNORM: return "dxt1";
  case plume::RenderFormat::BC2_UNORM: return "dxt3";
  case plume::RenderFormat::BC3_UNORM: return "dxt5";
  case plume::RenderFormat::BC4_UNORM: return "bc4";
  case plume::RenderFormat::BC5_UNORM: return "bc5";
  case plume::RenderFormat::R8_UNORM: return "r8";
  default: return "rgba8";
  }
}

// The legacy header every DDS reader understands, for what dumps produce.
DdsHeader DumpHeader(plume::RenderFormat fmt, u32 width, u32 height,
                     u32 mip_count) {
  DdsHeader h{};
  h.size = sizeof(DdsHeader);
  h.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat |
            kDdsdMipMapCount;
  h.width = width;
  h.height = height;
  h.mip_count = mip_count;
  h.pf.size = sizeof(DdsPixelFormat);
  h.caps = kDdsCapsTexture | (mip_count > 1 ? kDdsCapsComplex | kDdsCapsMipMap : 0);
  const bool compressed = plume::RenderFormatBlockWidth(fmt) > 1;
  h.flags |= compressed ? kDdsdLinearSize : kDdsdPitch;
  h.pitch_or_linear_size = compressed
                               ? PackedRowBytes(fmt, width) * BlockRows(fmt, height)
                               : PackedRowBytes(fmt, width);
  switch (fmt) {
  case plume::RenderFormat::BC1_UNORM: h.pf.flags = kDdpfFourCC; h.pf.fourcc = FourCC('D', 'X', 'T', '1'); break;
  case plume::RenderFormat::BC2_UNORM: h.pf.flags = kDdpfFourCC; h.pf.fourcc = FourCC('D', 'X', 'T', '3'); break;
  case plume::RenderFormat::BC3_UNORM: h.pf.flags = kDdpfFourCC; h.pf.fourcc = FourCC('D', 'X', 'T', '5'); break;
  case plume::RenderFormat::BC4_UNORM: h.pf.flags = kDdpfFourCC; h.pf.fourcc = FourCC('A', 'T', 'I', '1'); break;
  case plume::RenderFormat::BC5_UNORM: h.pf.flags = kDdpfFourCC; h.pf.fourcc = FourCC('A', 'T', 'I', '2'); break;
  case plume::RenderFormat::R8_UNORM:
    h.pf.flags = kDdpfLuminance;
    h.pf.rgb_bits = 8;
    h.pf.r_mask = 0xFF;
    break;
  default: // R8G8B8A8_UNORM
    h.pf.flags = kDdpfRgb | kDdpfAlphaPixels;
    h.pf.rgb_bits = 32;
    h.pf.r_mask = 0x000000FF;
    h.pf.g_mask = 0x0000FF00;
    h.pf.b_mask = 0x00FF0000;
    h.pf.a_mask = 0xFF000000;
    break;
  }
  return h;
}

// The host format a DDS holds, or UNKNOWN when it is not one sylpheed loads.
// sRGB variants load as UNORM: Xenos samples these textures without decoding.
plume::RenderFormat DdsFormat(const DdsHeader &h, const DdsHeaderDx10 *dx10) {
  if (dx10) {
    switch (dx10->dxgi_format) {
    case 71: case 72: return plume::RenderFormat::BC1_UNORM;
    case 74: case 75: return plume::RenderFormat::BC2_UNORM;
    case 77: case 78: return plume::RenderFormat::BC3_UNORM;
    case 80: return plume::RenderFormat::BC4_UNORM;
    case 83: return plume::RenderFormat::BC5_UNORM;
    case 98: case 99: return plume::RenderFormat::BC7_UNORM;
    case 28: case 29: return plume::RenderFormat::R8G8B8A8_UNORM;
    case 87: case 91: return plume::RenderFormat::B8G8R8A8_UNORM;
    case 61: return plume::RenderFormat::R8_UNORM;
    default: return plume::RenderFormat::UNKNOWN;
    }
  }
  const DdsPixelFormat &pf = h.pf;
  if (pf.flags & kDdpfFourCC) {
    switch (pf.fourcc) {
    case FourCC('D', 'X', 'T', '1'): return plume::RenderFormat::BC1_UNORM;
    case FourCC('D', 'X', 'T', '2'):
    case FourCC('D', 'X', 'T', '3'): return plume::RenderFormat::BC2_UNORM;
    case FourCC('D', 'X', 'T', '4'):
    case FourCC('D', 'X', 'T', '5'): return plume::RenderFormat::BC3_UNORM;
    case FourCC('A', 'T', 'I', '1'):
    case FourCC('B', 'C', '4', 'U'): return plume::RenderFormat::BC4_UNORM;
    case FourCC('A', 'T', 'I', '2'):
    case FourCC('B', 'C', '5', 'U'): return plume::RenderFormat::BC5_UNORM;
    default: return plume::RenderFormat::UNKNOWN;
    }
  }
  if ((pf.flags & kDdpfRgb) && pf.rgb_bits == 32) {
    if (pf.r_mask == 0x000000FF && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x00FF0000)
      return plume::RenderFormat::R8G8B8A8_UNORM;
    if (pf.r_mask == 0x00FF0000 && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x000000FF)
      return plume::RenderFormat::B8G8R8A8_UNORM;
  }
  if ((pf.flags & kDdpfLuminance) && pf.rgb_bits == 8)
    return plume::RenderFormat::R8_UNORM;
  return plume::RenderFormat::UNKNOWN;
}

// --- State ------------------------------------------------------------------

struct State {
  std::mutex mutex;
  bool indexed = false;
  std::unordered_map<u64, fs::path> replacements;
  std::unordered_set<u64> dumped;
  fs::path dump_dir;
};

State &state_() {
  static State s;
  return s;
}

fs::path UserDir() {
  return rex::Runtime::instance() ? rex::Runtime::instance()->user_data_root()
                                  : fs::path(".");
}

// The leading 16 hex digits of a file name, the hash it replaces.
bool ParseHash(const std::string &stem, u64 &out) {
  if (stem.size() < 16)
    return false;
  u64 v = 0;
  for (int i = 0; i < 16; ++i) {
    const char c = stem[i];
    u32 d;
    if (c >= '0' && c <= '9') d = u32(c - '0');
    else if (c >= 'a' && c <= 'f') d = u32(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = u32(c - 'A' + 10);
    else return false;
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

void IndexReplacementsLocked(State &s) {
  s.indexed = true;
  const fs::path dir = UserDir() / "textures" / "replace";
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec))
      continue;
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    u64 hash;
    if (ext == ".dds" && ParseHash(it->path().stem().string(), hash))
      s.replacements[hash] = it->path();
  }
  SYLPHEED_INFO("texture replacement: {} DDS file(s) in {}", s.replacements.size(),
           dir.string());
}

u64 ContentHash(plume::RenderFormat fmt, u32 guest_format, u32 width,
                u32 height, const BCMipLevel &top) {
  const u32 visible = PackedRowBytes(fmt, width);
  const u32 rows = BlockRows(fmt, height);
  const u32 pitch = top.row_width_texels / plume::RenderFormatBlockWidth(fmt) *
                    plume::RenderFormatSize(fmt);
  XXH3_state_t *st = XXH3_createState();
  XXH3_64bits_reset_withSeed(
      st, (u64(guest_format) << 32) | (u64(width) << 16) | u64(height));
  const auto *data = static_cast<const u8 *>(top.data);
  for (u32 y = 0; y < rows && size_t(y) * pitch + visible <= top.size; ++y)
    XXH3_64bits_update(st, data + size_t(y) * pitch, visible);
  const u64 hash = XXH3_64bits_digest(st);
  XXH3_freeState(st);
  return hash;
}

u8 Swizzled(const u8 *texel, plume::RenderSwizzle sel, u32 own) {
  switch (sel) {
  case plume::RenderSwizzle::ZERO: return 0;
  case plume::RenderSwizzle::ONE: return 0xFF;
  case plume::RenderSwizzle::R: return texel[0];
  case plume::RenderSwizzle::G: return texel[1];
  case plume::RenderSwizzle::B: return texel[2];
  case plume::RenderSwizzle::A: return texel[3];
  default: return texel[own];
  }
}

void DumpLocked(State &s, u64 hash, plume::RenderFormat fmt, u32 width,
                u32 height, const BCMipLevel *levels, u32 level_count,
                const plume::RenderComponentMapping &mapping) {
  if (!s.dumped.insert(hash).second)
    return;
  if (s.dump_dir.empty()) {
    s.dump_dir = UserDir() / "textures" / "dump";
    std::error_code ec;
    fs::create_directories(s.dump_dir, ec);
  }
  const fs::path path = s.dump_dir / std::format("{:016X}_{}x{}_{}.dds", hash,
                                                 width, height, FormatName(fmt));
  std::error_code ec;
  if (fs::exists(path, ec))
    return;
  FILE *f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return;
  const DdsHeader h = DumpHeader(fmt, width, height, level_count);
  std::fwrite(&kDdsMagic, 4, 1, f);
  std::fwrite(&h, sizeof(h), 1, f);
  const bool rgba8 = fmt == plume::RenderFormat::R8G8B8A8_UNORM;
  std::vector<u8> row;
  for (u32 i = 0; i < level_count; ++i) {
    const BCMipLevel &lv = levels[i];
    const u32 visible = PackedRowBytes(fmt, lv.width);
    const u32 rows = BlockRows(fmt, lv.height);
    const u32 pitch = lv.row_width_texels / plume::RenderFormatBlockWidth(fmt) *
                      plume::RenderFormatSize(fmt);
    const auto *data = static_cast<const u8 *>(lv.data);
    for (u32 y = 0; y < rows; ++y) {
      row.assign(visible, 0);
      if (size_t(y) * pitch + visible <= lv.size)
        std::memcpy(row.data(), data + size_t(y) * pitch, visible);
      // k_8_8_8_8 bakes in the fetch swizzle, so the dump shows true colours.
      if (rgba8) {
        for (u32 x = 0; x + 4 <= visible; x += 4) {
          const u8 in[4] = {row[x], row[x + 1], row[x + 2], row[x + 3]};
          row[x] = Swizzled(in, mapping.r, 0);
          row[x + 1] = Swizzled(in, mapping.g, 1);
          row[x + 2] = Swizzled(in, mapping.b, 2);
          row[x + 3] = Swizzled(in, mapping.a, 3);
        }
      }
      std::fwrite(row.data(), 1, row.size(), f);
    }
  }
  std::fclose(f);
}

GuestTexture *LoadReplacement(const fs::path &path,
                              const plume::RenderComponentMapping &mapping) {
  FILE *f = std::fopen(path.string().c_str(), "rb");
  if (!f)
    return nullptr;
  std::vector<u8> file;
  std::fseek(f, 0, SEEK_END);
  file.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  const size_t got = std::fread(file.data(), 1, file.size(), f);
  std::fclose(f);
  auto fail = [&](const char *why) -> GuestTexture * {
    SYLPHEED_WARN("texture replacement {}: {}", path.filename().string(), why);
    return nullptr;
  };
  if (got != file.size() || file.size() < 4 + sizeof(DdsHeader))
    return fail("not a DDS file");
  u32 magic;
  DdsHeader h;
  std::memcpy(&magic, file.data(), 4);
  std::memcpy(&h, file.data() + 4, sizeof(h));
  if (magic != kDdsMagic || h.size != sizeof(DdsHeader))
    return fail("not a DDS file");
  size_t offset = 4 + sizeof(DdsHeader);
  DdsHeaderDx10 dx10{};
  const bool has_dx10 = (h.pf.flags & kDdpfFourCC) &&
                        h.pf.fourcc == FourCC('D', 'X', '1', '0');
  if (has_dx10) {
    if (file.size() < offset + sizeof(dx10))
      return fail("truncated DX10 header");
    std::memcpy(&dx10, file.data() + offset, sizeof(dx10));
    offset += sizeof(dx10);
    if (dx10.resource_dimension != 3 || dx10.array_size > 1)
      return fail("only single 2D textures are supported");
  }
  if (h.caps2 & (kDdsCaps2Cubemap | kDdsCaps2Volume))
    return fail("only 2D textures are supported");
  const plume::RenderFormat fmt = DdsFormat(h, has_dx10 ? &dx10 : nullptr);
  if (fmt == plume::RenderFormat::UNKNOWN)
    return fail("unsupported format (use BC1-5, BC7, RGBA8, BGRA8 or R8)");
  if (!h.width || !h.height || h.width > 16384 || h.height > 16384)
    return fail("bad size");
  const u32 levels = std::max(1u, (h.flags & kDdsdMipMapCount) ? h.mip_count : 1u);

  // Repack each level into 256-byte aligned rows for the upload footprint.
  std::vector<std::vector<u8>> bufs(levels);
  std::vector<BCMipLevel> mips(levels);
  const u32 bw = plume::RenderFormatBlockWidth(fmt);
  const u32 bsize = plume::RenderFormatSize(fmt);
  for (u32 i = 0; i < levels; ++i) {
    const u32 w = std::max(h.width >> i, 1u);
    const u32 hgt = std::max(h.height >> i, 1u);
    const u32 packed = PackedRowBytes(fmt, w);
    const u32 rows = BlockRows(fmt, hgt);
    if (offset + size_t(packed) * rows > file.size())
      return fail("truncated mip chain");
    const u32 pitch = (packed + 255u) & ~255u;
    bufs[i].assign(size_t(pitch) * rows, 0);
    for (u32 y = 0; y < rows; ++y)
      std::memcpy(bufs[i].data() + size_t(y) * pitch,
                  file.data() + offset + size_t(y) * packed, packed);
    offset += size_t(packed) * rows;
    mips[i] = {bufs[i].data(), bufs[i].size(), w, hgt, pitch / bsize * bw};
  }
  return BuildHostTexture2DMips(fmt, h.width, h.height, mips.data(), levels,
                                &mapping);
}

} // namespace

GuestTexture *ReplaceOrDumpTexture(u32 guest_format, u32 width, u32 height,
                                   const BCMipLevel *levels, u32 level_count,
                                   const plume::RenderComponentMapping &mapping) {
  const bool replace = REXCVAR_GET(sylpheed_texture_replace);
  const bool dump = REXCVAR_GET(sylpheed_texture_dump);
  if ((!replace && !dump) || !levels || !level_count)
    return nullptr;
  plume::RenderFormat fmt;
  if (!BCFormatFromGuestByte(guest_format, fmt))
    return nullptr;

  State &s = state_();
  fs::path replacement;
  u64 hash = 0;
  {
    std::lock_guard lock(s.mutex);
    if (replace && !s.indexed)
      IndexReplacementsLocked(s);
    if (!dump && s.replacements.empty())
      return nullptr;
    hash = ContentHash(fmt, guest_format, width, height, levels[0]);
    if (replace) {
      if (auto it = s.replacements.find(hash); it != s.replacements.end())
        replacement = it->second;
    }
    if (dump)
      DumpLocked(s, hash, fmt, width, height, levels, level_count, mapping);
  }
  if (replacement.empty())
    return nullptr;
  // A k_8_8_8_8 dump already carries the fetch swizzle.
  const plume::RenderComponentMapping replacement_mapping =
      guest_format == u32(xe::TextureFormat::k_8_8_8_8)
          ? plume::RenderComponentMapping{}
          : mapping;
  GuestTexture *tex = LoadReplacement(replacement, replacement_mapping);
  if (tex)
    SYLPHEED_DEBUG("texture {:016X} replaced by {}", hash,
              replacement.filename().string());
  return tex;
}

} // namespace sylpheed::gpu
