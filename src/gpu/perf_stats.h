// sylpheed - ReXGlue Recompiled Project
//
// Log-based frame and hot-path timings, for builds without a Tracy viewer.
// sylpheed_perf_stats=N reports every N presents: the present-to-present interval
// distribution, and per counter its count, total and worst time and bytes.

#pragma once

#include <chrono>

#include <rex/types.h>

namespace sylpheed::gpu::perf {

enum class Counter : u32 {
  PsoBuild,      // a pipeline built on the drawing thread
  ShaderLink,    // a runtime DXC link (no prelinked variant)
  TextureMirror, // a native texture mirror built or rebuilt
  TexelHash,     // a linear texture's per-frame texel hash
  BufferHash,    // a vertex/index buffer's per-frame rehash
  BufferUpload,  // a vertex/index buffer mirror re-uploaded
  Draw,          // a guest draw dispatched
  FenceWait,     // Present waiting for the GPU to free the next frame slot
  PaceSleep,     // Present sleeping to the fps limit
  kCount,
};

bool Enabled();
void Add(Counter counter, u64 ns, u64 bytes = 0);

// The display's vblanks, from a thread waiting on them, and the moment each
// present was queued. A frame shows from the first vblank after its present,
// so the vblanks between consecutive presents are its refreshes on screen.
void NoteVBlank();
void NotePresentQueued();

// Called once at the end of every Present.
void OnPresent();

// Times its own lifetime into a counter, when the stats are on.
class Scope {
public:
  explicit Scope(Counter counter, u64 bytes = 0)
      : counter_(counter), bytes_(bytes), on_(Enabled()) {
    if (on_)
      start_ = std::chrono::steady_clock::now();
  }
  ~Scope() {
    if (on_)
      Add(counter_, u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_)
                            .count()),
          bytes_);
  }
  void SetBytes(u64 bytes) { bytes_ = bytes; }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  Counter counter_;
  u64 bytes_;
  bool on_;
  std::chrono::steady_clock::time_point start_{};
};

} // namespace sylpheed::gpu::perf
