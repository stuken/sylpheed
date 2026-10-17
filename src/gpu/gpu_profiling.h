/**
 * @file    gpu/gpu_profiling.h
 * @brief   Tracy GPU zones. Include this only to place a SYLPHEED_GPU_ZONE. CPU
 *          zones and frame marks live in core/profiling.h, which does not
 *          drag in TracyD3D12.hpp.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#if defined(SYLPHEED_COMMON_TU)
#error "backend-conditional: move this TU to reblue_backend_only in src/CMakeLists.txt, or use core/profiling.h"
#endif

#include <rex/types.h>

#include "core/profiling.h"

#if defined(REXGLUE_ENABLE_PROFILING)

#if defined(SYLPHEED_D3D12)

#include <tracy/TracyD3D12.hpp>

namespace sylpheed::gpu {
void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue);
TracyD3D12Ctx GpuProfilerCtx();
void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd);
ID3D12GraphicsCommandList *GpuProfilerCommandList();
} // namespace sylpheed::gpu

#define SYLPHEED_GPU_ZONE(name)                                                      \
  ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted);                      \
  static constexpr tracy::SourceLocationData TracyConcat(                      \
      __tracy_gpu_sloc, TracyLine){name, TracyFunction, TracyFile,             \
                                   (u32)TracyLine, 0};                         \
  tracy::D3D12ZoneScope TracyConcat(__tracy_gpu_zone, TracyLine) {             \
    sylpheed::gpu::GpuProfilerCtx(), sylpheed::gpu::GpuProfilerCommandList(),              \
        &TracyConcat(__tracy_gpu_sloc, TracyLine), TracyIsStarted              \
  }

#else // profiling without D3D12: GPU zones degrade to CPU zones (Tracy-Vk
      // deferred)

#define SYLPHEED_GPU_ZONE(name) ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted)

#endif // SYLPHEED_D3D12

#else

#define SYLPHEED_GPU_ZONE(name) ((void)0)

#endif
