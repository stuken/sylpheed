// sylpheed - ReXGlue Recompiled Project
//
// Pipeline precache. A cold pipeline compile takes 10-20 ms on the drawing
// thread (a first run, or after a driver update empties the driver's cache),
// and the boot and first mission need about seventy. So the
// pipelines recorded sessions needed are compiled into the binary
// (cache/pipeline_state_cache.h) and built on background threads as soon as
// their shaders (all created at boot) and vertex declaration (created as
// models load) exist. Each entry carries the shaders' and the declaration's
// content hashes in its pointer fields.
//
// sylpheed_pso_capture writes the compiled-in list plus every pipeline a draw still
// had to compile to <cache>/pipeline_state_cache.h, sorted, ready to replace
// the one in the source tree.

#include "gpu/pipeline/pso_recorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "core/settings.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"

REXCVAR_DEFINE_BOOL(sylpheed_pso_precache, true, kCvarGroup,
                    "Compile the recorded pipelines in the background before "
                    "draws need them");
REXCVAR_DEFINE_BOOL(sylpheed_pso_capture, false, kCvarGroup,
                    "Write the precache list plus every pipeline a draw had "
                    "to compile to pipeline_state_cache.h in the cache folder");

namespace sylpheed::gpu {

namespace {

// std::vector rather than an array, so an empty list still compiles.
const std::vector<PipelineState> g_compiled_in = {
#include "gpu/pipeline/cache/pipeline_state_cache.h"
};

u64 Bits(const void *p) { return u64(reinterpret_cast<uintptr_t>(p)); }

template <typename T> T *FromBits(u64 v) {
  return reinterpret_cast<T *>(uintptr_t(v));
}

u64 ShaderHash(const GuestShader *s) {
  return s && s->shaderCacheEntry ? s->shaderCacheEntry->hash : 0;
}

// A sanitized live state as it means on any machine and run: content hashes
// in the pointer fields, and the device's depth format folded back to the
// generic one SanitizePipelineState maps from.
PipelineState ToPortable(const PipelineState &live) {
  PipelineState p = live;
  p.vertexShader = FromBits<GuestShader>(ShaderHash(live.vertexShader));
  p.pixelShader = FromBits<GuestShader>(ShaderHash(live.pixelShader));
  p.vertexDeclaration = FromBits<GuestVertexDeclaration>(
      live.vertexDeclaration ? live.vertexDeclaration->hash : 0);
  if (plume::RenderFormatIsStencil(p.depthStencilFormat))
    p.depthStencilFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
  // The shadow quads' slope bias tracks the camera every frame. With dynamic
  // depth bias (Vulkan, D3D12 on Windows 11 24H2+) it is not part of the
  // pipeline at all; where it is baked, the values never repeat reliably
  // enough to be worth listing. Either way the list holds the bias-free entry.
  p.depthBias = 0;
  p.slopeScaledDepthBias = 0.0f;
  return p;
}

// --- Replay ---------------------------------------------------------------

std::mutex g_replay_mutex;
std::condition_variable g_replay_cv;
std::vector<PipelineState> g_pending; // portable, missing a shader or decl
std::deque<PipelineState> g_queue;    // resolved, waiting for a worker
std::vector<std::thread> g_workers;
bool g_replay_started = false;
bool g_stopping = false;
u32 g_busy = 0;
u32 g_compiled = 0;
std::chrono::steady_clock::time_point g_replay_start;

// Fills 'out' with the live state for a portable entry, or returns false while
// one of its shaders or its declaration does not exist yet.
bool Resolve(const PipelineState &entry, PipelineState &out) {
  GuestShader *vs = FindGuestShaderByHash(Bits(entry.vertexShader));
  if (!vs)
    return false;
  GuestShader *ps = nullptr;
  if (const u64 ps_hash = Bits(entry.pixelShader)) {
    ps = FindGuestShaderByHash(ps_hash);
    if (!ps)
      return false;
  }
  GuestVertexDeclaration *decl =
      FindVertexDeclarationByHash(Bits(entry.vertexDeclaration));
  if (!decl)
    return false;
  out = entry;
  out.vertexShader = vs;
  out.pixelShader = ps;
  out.vertexDeclaration = decl;
  SanitizePipelineState(out);
  return true;
}

void DrainPendingLocked() {
  bool queued = false;
  for (size_t i = 0; i < g_pending.size();) {
    PipelineState live;
    if (Resolve(g_pending[i], live)) {
      g_queue.push_back(live);
      g_pending[i] = g_pending.back();
      g_pending.pop_back();
      queued = true;
    } else {
      ++i;
    }
  }
  if (queued)
    g_replay_cv.notify_all();
}

void Worker() {
  std::unique_lock lock(g_replay_mutex);
  for (;;) {
    g_replay_cv.wait(lock, [] { return g_stopping || !g_queue.empty(); });
    if (g_stopping)
      return;
    const PipelineState state = g_queue.front();
    g_queue.pop_front();
    ++g_busy;
    lock.unlock();
    GetOrCreatePipeline(state);
    lock.lock();
    --g_busy;
    ++g_compiled;
    if (g_queue.empty() && !g_busy)
      SYLPHEED_INFO("pso precache: {} of {} pipelines compiled after {:.0f} ms, {} "
               "waiting for their shaders or vertex declaration",
               g_compiled, g_compiled_in.size(),
               std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - g_replay_start)
                   .count(),
               g_pending.size());
  }
}

// --- Capture --------------------------------------------------------------

std::mutex g_capture_mutex;
std::unordered_set<u64> g_known; // HashPipelineState of portable entries
std::vector<PipelineState> g_captured;
u32 g_misses = 0;

void InitKnownLocked() {
  if (!g_known.empty() || g_compiled_in.empty())
    return;
  for (const PipelineState &e : g_compiled_in)
    g_known.insert(HashPipelineState(e));
}

template <typename T>
void Field(std::string &out, const char *name, const T &value,
           const T &fallback, const std::string &expr) {
  if (value != fallback)
    out += std::format("\n     .{} = {},", name, expr);
}

template <typename E>
void EnumField(std::string &out, const char *name, const char *type, E value,
               E fallback) {
  Field(out, name, value, fallback, std::format("{}({})", type, int(value)));
}

void BoolField(std::string &out, const char *name, bool value, bool fallback) {
  Field(out, name, value, fallback, std::string(value ? "true" : "false"));
}

// An exact float literal (hex), sign outside the 0x.
std::string FloatLiteral(float v) {
  return std::format("{}0x{:a}f", v < 0 ? "-" : "", v < 0 ? -v : v);
}

// One designated initializer naming every field that differs from a default
// PipelineState, in declaration order.
std::string EmitEntry(const PipelineState &s) {
  static_assert(sizeof(PipelineState) == 154,
                "PipelineState changed: update EmitEntry and recapture");
  const PipelineState d{};
  std::string out = "    {";
  auto ptr = [&](const char *name, const char *type, const void *v) {
    if (v)
      out += std::format("\n     .{} = reinterpret_cast<{} *>(0x{:016X}ull),",
                         name, type, Bits(v));
  };
  ptr("vertexShader", "GuestShader", s.vertexShader);
  ptr("pixelShader", "GuestShader", s.pixelShader);
  ptr("vertexDeclaration", "GuestVertexDeclaration", s.vertexDeclaration);
  BoolField(out, "instancing", s.instancing, d.instancing);
  BoolField(out, "zEnable", s.zEnable, d.zEnable);
  BoolField(out, "zWriteEnable", s.zWriteEnable, d.zWriteEnable);
  BoolField(out, "stencilEnable", s.stencilEnable, d.stencilEnable);
  BoolField(out, "stencilTwoSided", s.stencilTwoSided, d.stencilTwoSided);
  EnumField(out, "srcBlend", "plume::RenderBlend", s.srcBlend, d.srcBlend);
  EnumField(out, "destBlend", "plume::RenderBlend", s.destBlend, d.destBlend);
  EnumField(out, "cullMode", "plume::RenderCullMode", s.cullMode, d.cullMode);
  EnumField(out, "fillMode", "plume::RenderFillMode", s.fillMode, d.fillMode);
  EnumField(out, "frontFace", "plume::RenderFrontFace", s.frontFace,
            d.frontFace);
  EnumField(out, "zFunc", "plume::RenderComparisonFunction", s.zFunc, d.zFunc);
  EnumField(out, "stencilFunc", "plume::RenderComparisonFunction",
            s.stencilFunc, d.stencilFunc);
  EnumField(out, "stencilFail", "plume::RenderStencilOp", s.stencilFail,
            d.stencilFail);
  EnumField(out, "stencilZFail", "plume::RenderStencilOp", s.stencilZFail,
            d.stencilZFail);
  EnumField(out, "stencilPass", "plume::RenderStencilOp", s.stencilPass,
            d.stencilPass);
  EnumField(out, "stencilFuncCCW", "plume::RenderComparisonFunction",
            s.stencilFuncCCW, d.stencilFuncCCW);
  EnumField(out, "stencilFailCCW", "plume::RenderStencilOp", s.stencilFailCCW,
            d.stencilFailCCW);
  EnumField(out, "stencilZFailCCW", "plume::RenderStencilOp",
            s.stencilZFailCCW, d.stencilZFailCCW);
  EnumField(out, "stencilPassCCW", "plume::RenderStencilOp", s.stencilPassCCW,
            d.stencilPassCCW);
  Field(out, "stencilMask", s.stencilMask, d.stencilMask,
        std::format("0x{:X}", u32(s.stencilMask)));
  Field(out, "stencilWriteMask", s.stencilWriteMask, d.stencilWriteMask,
        std::format("0x{:X}", u32(s.stencilWriteMask)));
  Field(out, "stencilRef", s.stencilRef, d.stencilRef,
        std::format("0x{:X}", u32(s.stencilRef)));
  BoolField(out, "alphaBlendEnable", s.alphaBlendEnable, d.alphaBlendEnable);
  EnumField(out, "blendOp", "plume::RenderBlendOperation", s.blendOp,
            d.blendOp);
  Field(out, "slopeScaledDepthBias", s.slopeScaledDepthBias,
        d.slopeScaledDepthBias, FloatLiteral(s.slopeScaledDepthBias));
  Field(out, "depthBias", s.depthBias, d.depthBias,
        std::to_string(s.depthBias));
  EnumField(out, "srcBlendAlpha", "plume::RenderBlend", s.srcBlendAlpha,
            d.srcBlendAlpha);
  EnumField(out, "destBlendAlpha", "plume::RenderBlend", s.destBlendAlpha,
            d.destBlendAlpha);
  EnumField(out, "blendOpAlpha", "plume::RenderBlendOperation", s.blendOpAlpha,
            d.blendOpAlpha);
  Field(out, "colorWriteEnable", s.colorWriteEnable, d.colorWriteEnable,
        std::format("0x{:X}", s.colorWriteEnable));
  EnumField(out, "primitiveTopology", "plume::RenderPrimitiveTopology",
            s.primitiveTopology, d.primitiveTopology);
  {
    int last = -1;
    for (int i = 0; i < 16; ++i)
      if (s.vertexStrides[i])
        last = i;
    if (last >= 0) {
      std::string strides;
      for (int i = 0; i <= last; ++i)
        strides += std::format("{}{}", i ? ", " : "", u32(s.vertexStrides[i]));
      out += std::format("\n     .vertexStrides = {{{}}},", strides);
    }
  }
  EnumField(out, "renderTargetFormat", "plume::RenderFormat",
            s.renderTargetFormat, d.renderTargetFormat);
  EnumField(out, "depthStencilFormat", "plume::RenderFormat",
            s.depthStencilFormat, d.depthStencilFormat);
  Field(out, "sampleCount", s.sampleCount, d.sampleCount,
        std::format("plume::RenderSampleCounts({})", u32(s.sampleCount)));
  BoolField(out, "enableAlphaToCoverage", s.enableAlphaToCoverage,
            d.enableAlphaToCoverage);
  Field(out, "specConstants", s.specConstants, d.specConstants,
        std::format("0x{:X}", s.specConstants));
  if (out.back() == ',')
    out.pop_back();
  out += "},\n";
  return out;
}

void WriteCaptureLocked() {
  std::vector<PipelineState> all(g_compiled_in.begin(), g_compiled_in.end());
  all.insert(all.end(), g_captured.begin(), g_captured.end());
  std::sort(all.begin(), all.end(),
            [](const PipelineState &a, const PipelineState &b) {
              return HashPipelineState(a) < HashPipelineState(b);
            });
  std::string text = std::format(
      "// {} pipeline(s), written by sylpheed_pso_capture (gpu/pipeline/"
      "pso_recorder.cpp).\n\n",
      all.size());
  for (const PipelineState &e : all)
    text += EmitEntry(e);

  std::filesystem::path dir;
  if (auto *rt = rex::Runtime::instance())
    dir = rt->cache_root();
  const auto path = dir / "pipeline_state_cache.h";
  std::error_code ec;
  if (!dir.empty())
    std::filesystem::create_directories(dir, ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(text.data(), std::streamsize(text.size()));
  if (!f)
    SYLPHEED_WARN("pso capture: writing {} failed", path.string());
  else if (g_captured.size() == 1)
    SYLPHEED_INFO("pso capture: writing {}", path.string());
}

} // namespace

void RecordPipelineState(const PipelineState &state, u32 renderPassId,
                         bool builtOnRenderThread) {
  if (!builtOnRenderThread)
    return;
  const PipelineState portable = ToPortable(state);
  const u64 key = HashPipelineState(portable);
  std::lock_guard lock(g_capture_mutex);
  InitKnownLocked();
  // Listed: its background compile just lost the race to this draw.
  if (!g_known.insert(key).second)
    return;
  ++g_misses;
  SYLPHEED_DEV_INFO("pso: compiled on the drawing thread, not in the precache "
               "list ({} so far): pass {} vs 0x{:016X} ps 0x{:016X} decl "
               "0x{:016X}",
               g_misses, renderPassId, Bits(portable.vertexShader),
               Bits(portable.pixelShader), Bits(portable.vertexDeclaration));
  if (!REXCVAR_GET(sylpheed_pso_capture) || !portable.vertexShader)
    return;
  g_captured.push_back(portable);
  WriteCaptureLocked();
}

void ReplayBootCache() {
  if (!REXCVAR_GET(sylpheed_pso_precache) || g_compiled_in.empty())
    return;
  std::lock_guard lock(g_replay_mutex);
  if (g_replay_started)
    return;
  g_replay_started = true;
  g_replay_start = std::chrono::steady_clock::now();
  g_pending = g_compiled_in;
  // Pipeline compiles are CPU bound in the driver. A few threads finish a
  // mission's worth during its loading without starving the game.
  const u32 threads =
      std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  for (u32 i = 0; i < threads; ++i)
    g_workers.emplace_back(Worker);
  DrainPendingLocked();
  SYLPHEED_INFO("pso precache: {} pipelines listed, {} threads",
           g_compiled_in.size(), threads);
}

void ShutdownPsoRecorder() {
  {
    std::lock_guard lock(g_replay_mutex);
    g_stopping = true;
    g_queue.clear();
  }
  g_replay_cv.notify_all();
  for (auto &t : g_workers)
    if (t.joinable())
      t.join();
  g_workers.clear();
}

void OnShaderCreated(u64) {
  std::lock_guard lock(g_replay_mutex);
  if (g_replay_started && !g_stopping && !g_pending.empty())
    DrainPendingLocked();
}

void OnVertexDeclarationCreated(u64) {
  std::lock_guard lock(g_replay_mutex);
  if (g_replay_started && !g_stopping && !g_pending.empty())
    DrainPendingLocked();
}

} // namespace sylpheed::gpu
