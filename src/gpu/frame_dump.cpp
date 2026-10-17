// sylpheed - ReXGlue Recompiled Project
//
// sylpheed_dump_frames="N,M,..." writes what those present frames show (its source render
// target) to <user data>/sylpheed_frame_N.bmp. A bring-up aid: it records its own
// command list right after Present submits, so queue order puts the copy
// behind the frame's draws, and waits for it synchronously.

#include "gpu/frame_dump.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "core/settings.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"
#include "platform/dump_schedule.h"

REXCVAR_DEFINE_STRING(sylpheed_dump_frames, "", kCvarGroup,
                      "Comma-separated present frame numbers to write as "
                      "sylpheed_frame_N.bmp in the user data folder");
REXCVAR_DEFINE_INT32(sylpheed_screenshot_every, 0, kCvarGroup,
                     "Write every Nth presented frame to <user data>/screens "
                     "(0 = off)");

namespace sylpheed::gpu {

namespace {

float HalfToFloat(u16 h) {
  const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  float v;
  if (exp == 0)
    v = std::ldexp(static_cast<float>(man), -24);
  else if (exp == 31)
    v = man ? 0.0f : 65504.0f;
  else
    v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

u8 ToByte(float v) {
  v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return static_cast<u8>(v * 255.0f + 0.5f);
}

bool WriteBmp(const std::filesystem::path &path, u32 width, u32 height,
              const std::vector<u8> &bgra) {
  FILE *f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const u32 image = width * height * 4;
  u8 header[54] = {'B', 'M'};
  auto put32 = [&](int off, u32 v) { std::memcpy(header + off, &v, 4); };
  auto put16 = [&](int off, u16 v) { std::memcpy(header + off, &v, 2); };
  put32(2, 54 + image);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, static_cast<u32>(-static_cast<i32>(height))); // top-down
  put16(26, 1);
  put16(28, 32);
  put32(34, image);
  std::fwrite(header, 1, sizeof(header), f);
  std::fwrite(bgra.data(), 1, image, f);
  std::fclose(f);
  return true;
}

bool DumpTexture(VideoState &s, GuestTexture *rt,
                 const std::filesystem::path &path) {
  if (!rt || !rt->texture)
    return false;
  const u32 width = rt->width, height = rt->height;
  u32 bpp = 0;
  switch (rt->format) {
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::R32_FLOAT:
    bpp = 4;
    break;
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
    bpp = 8;
    break;
  default:
    SYLPHEED_WARN("sylpheed_dump_frames: unsupported format {} for {}",
             static_cast<u32>(rt->format), path.string());
    return false;
  }
  // Placed footprints need 256-byte aligned rows.
  const u32 row_pixels = ((width * bpp + 255) & ~255u) / bpp;
  const u64 size = u64(row_pixels) * bpp * height;
  auto readback =
      s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(size));
  auto list = s.queue->createCommandList();
  auto fence = s.device->createCommandFence();
  if (!readback || !list || !fence)
    return false;

  const plume::RenderTextureLayout was =
      rt->layout == plume::RenderTextureLayout::UNKNOWN
          ? plume::RenderTextureLayout::SHADER_READ
          : rt->layout;
  list->begin();
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(
                     rt->texture, plume::RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(
          readback.get(), rt->format, width, height, 1, row_pixels),
      plume::RenderTextureCopyLocation::Subresource(rt->texture));
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(rt->texture, was));
  list->end();
  const plume::RenderCommandList *lists[] = {list.get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
  s.queue->waitForCommandFence(fence.get());

  const auto *src = static_cast<const u8 *>(readback->map());
  if (!src)
    return false;
  std::vector<u8> bgra(size_t(width) * height * 4);
  // Small float targets (luminance reductions) are logged by value too: as
  // pixels they only show whether they clip.
  const bool r32f = rt->format == plume::RenderFormat::R32_FLOAT;
  std::string values;
  for (u32 y = 0; y < height; ++y) {
    const u8 *row = src + size_t(y) * row_pixels * bpp;
    u8 *out = bgra.data() + size_t(y) * width * 4;
    for (u32 x = 0; x < width; ++x, out += 4) {
      if (r32f) {
        float v;
        std::memcpy(&v, row + x * 4, 4);
        out[0] = out[1] = out[2] = ToByte(v);
        out[3] = 255;
        if (width * height <= 64)
          values += std::format(" {:.6g}", v);
      } else if (rt->format == plume::RenderFormat::R16G16B16A16_UNORM) {
        u16 h[4];
        std::memcpy(h, row + x * 8, 8);
        out[0] = ToByte(h[2] / 65535.0f);
        out[1] = ToByte(h[1] / 65535.0f);
        out[2] = ToByte(h[0] / 65535.0f);
        out[3] = ToByte(h[3] / 65535.0f);
      } else if (bpp == 8) {
        u16 h[4];
        std::memcpy(h, row + x * 8, 8);
        out[0] = ToByte(HalfToFloat(h[2]));
        out[1] = ToByte(HalfToFloat(h[1]));
        out[2] = ToByte(HalfToFloat(h[0]));
        out[3] = ToByte(HalfToFloat(h[3]));
      } else if (rt->format == plume::RenderFormat::R8G8B8A8_UNORM) {
        out[0] = row[x * 4 + 2];
        out[1] = row[x * 4 + 1];
        out[2] = row[x * 4 + 0];
        out[3] = row[x * 4 + 3];
      } else {
        out[0] = row[x * 4 + 0];
        out[1] = row[x * 4 + 1];
        out[2] = row[x * 4 + 2];
        out[3] = row[x * 4 + 3];
      }
    }
  }
  readback->unmap();
  if (!values.empty())
    SYLPHEED_INFO("sylpheed_dump_frames: {} ={}", path.filename().string(), values);
  return WriteBmp(path, width, height, bgra);
}

} // namespace

void MaybeDumpFrame(VideoState &s, GuestTexture *rt) {
  static u32 s_frame = 0;
  static platform::DumpSchedule s_schedule(REXCVAR_GET(sylpheed_dump_frames));
  ++s_frame;
  std::filesystem::path dir = rex::Runtime::instance()
                                  ? rex::Runtime::instance()->user_data_root()
                                  : std::filesystem::path(".");
  std::error_code ec;
  // sylpheed_screenshot_every: just the presented frame, cheap enough to
  // follow a scripted run through its menus.
  if (const i32 every = REXCVAR_GET(sylpheed_screenshot_every);
      every > 0 && s_frame % u32(every) == 0) {
    std::filesystem::create_directories(dir / "screens", ec);
    DumpTexture(s, rt, dir / "screens" / std::format("frame_{:06}.bmp", s_frame));
  }
  if (s_schedule.Empty())
    return;
  const auto due = s_schedule.Due(s_frame);
  if (!due)
    return;
  const std::string &wanted = *due;

  std::filesystem::create_directories(dir, ec);
  const auto path = dir / ("sylpheed_frame_" + wanted + ".bmp");
  if (DumpTexture(s, rt, path))
    SYLPHEED_INFO("sylpheed_dump_frames: wrote {} ({}x{})", path.string(), rt->width,
             rt->height);
  // Every render target the guest resolved, as it stands after this frame.
  ForEachResolveTarget([&](u32 guest_va, GuestTexture *tex) {
    const auto rt_path = dir / std::format("sylpheed_frame_{}_rt_{:08X}_{}x{}.bmp",
                                           wanted, guest_va, tex->width,
                                           tex->height);
    DumpTexture(s, tex, rt_path);
  });
}

} // namespace sylpheed::gpu
