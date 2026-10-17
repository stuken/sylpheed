// sylpheed - ReXGlue Recompiled Project

#include "gpu/guest_buffers.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/byte_swap.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/resources.h"
#include "gpu/perf_stats.h"

namespace sylpheed::gpu {

namespace {

struct Mirror {
  std::unique_ptr<GuestBuffer> buffer;
  u64 hash = 0;
  u64 verified_frame = ~0ull;
};

std::mutex g_mutex;
std::unordered_map<u64, Mirror> g_mirrors; // (base_va, type) -> mirror
std::vector<std::unique_ptr<plume::RenderBuffer>> g_graveyard[kNumFrames];
// Evicted mirrors keep their GuestBuffer alive until the slot drains too, since
// Video's index binding holds the GuestBuffer pointer itself.
std::vector<std::unique_ptr<GuestBuffer>> g_dead_mirrors[kNumFrames];
u64 g_frame = 0;

u64 Key(u32 base_va, ResourceType type) {
  return (u64(static_cast<u32>(type)) << 32) | base_va;
}

std::unique_ptr<plume::RenderBuffer> Upload(plume::RenderDevice *device,
                                            const u8 *src, u32 size,
                                            bool is_index, u32 element_size) {
  const plume::RenderHeapType heap =
      GeometryHeapType(device, GeometryClass::Static);
  const plume::RenderBufferDesc desc =
      is_index ? plume::RenderBufferDesc::IndexBuffer(size, heap)
               : plume::RenderBufferDesc::VertexBuffer(size, heap);
  auto buffer = CreateHostBuffer(device, desc,
                                 is_index ? "guest-ib" : "guest-vb");
  if (!buffer)
    return nullptr;
  void *mapped = buffer->map();
  if (!mapped)
    return nullptr;
  ByteSwapElements(mapped, src, size, element_size);
  buffer->unmap();
  return buffer;
}

} // namespace

GuestBuffer *MirrorGuestBuffer(u32 base_va, u32 size, ResourceType type,
                               u32 element_size, plume::RenderFormat format) {
  if (!base_va || !size)
    return nullptr;
  const auto *src = sylpheed::mem::at<const u8>(base_va);
  auto *device = Video::HostDevice();
  if (!src || !device)
    return nullptr;

  std::lock_guard lock(g_mutex);
  Mirror &m = g_mirrors[Key(base_va, type)];
  const bool same_shape =
      m.buffer && m.buffer->dataSize == size && m.buffer->format == format;
  if (same_shape && m.verified_frame == g_frame)
    return m.buffer.get();

  u64 hash;
  {
    perf::Scope timer(perf::Counter::BufferHash, size);
    hash = XXH3_64bits(src, size);
  }
  if (same_shape && hash == m.hash) {
    m.verified_frame = g_frame;
    return m.buffer.get();
  }

  std::unique_ptr<plume::RenderBuffer> fresh;
  {
    perf::Scope timer(perf::Counter::BufferUpload, size);
    fresh = Upload(device, src, size, type == ResourceType::IndexBuffer,
                   element_size);
  }
  if (!fresh) {
    SYLPHEED_ERROR("MirrorGuestBuffer: upload failed (0x{:08X}, {} bytes)", base_va,
              size);
    return m.buffer.get(); // stale beats nothing
  }
  if (!m.buffer) {
    m.buffer = std::make_unique<GuestBuffer>(type);
  } else if (m.buffer->buffer) {
    // Bound views may still name the old buffer in the recording list.
    Video::ScrubBufferBindings(m.buffer->buffer.get());
    g_graveyard[Video::RetireSlot("guest buffer mirror")].push_back(
        std::move(m.buffer->buffer));
  }
  m.buffer->buffer = std::move(fresh);
  m.buffer->guestMirrorVa = base_va;
  m.buffer->dataSize = size;
  m.buffer->format = format;
  m.hash = hash;
  m.verified_frame = g_frame;
  return m.buffer.get();
}

bool EvictGuestBuffer(u32 base_va, ResourceType type) {
  std::unique_ptr<GuestBuffer> dead;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_mirrors.find(Key(base_va, type));
    if (it == g_mirrors.end())
      return false;
    dead = std::move(it->second.buffer);
    g_mirrors.erase(it);
  }
  if (!dead)
    return true;
  Video::ScrubBufferBindings(dead->buffer.get());
  Video::ForgetIndices(dead.get());
  std::lock_guard lock(g_mutex);
  g_dead_mirrors[Video::RetireSlot("evicted guest buffer")].push_back(
      std::move(dead));
  return true;
}

u32 EvictGuestBuffersInRange(u32 begin, u32 end) {
  std::vector<std::pair<u32, ResourceType>> dead;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[key, m] : g_mirrors) {
      const u32 base = u32(key);
      if (base >= begin && base < end)
        dead.emplace_back(base, static_cast<ResourceType>(key >> 32));
    }
  }
  for (const auto &[base, type] : dead)
    EvictGuestBuffer(base, type);
  return u32(dead.size());
}

void AdvanceGuestBufferFrame() {
  std::lock_guard lock(g_mutex);
  ++g_frame;
}

u64 GuestMirrorFrame() {
  std::lock_guard lock(g_mutex);
  return g_frame;
}

void DrainGuestBufferMirrors(u32 slot) {
  std::vector<std::unique_ptr<plume::RenderBuffer>> dead;
  std::vector<std::unique_ptr<GuestBuffer>> dead_mirrors;
  {
    std::lock_guard lock(g_mutex);
    dead.swap(g_graveyard[slot]);
    dead_mirrors.swap(g_dead_mirrors[slot]);
  }
}

} // namespace sylpheed::gpu
