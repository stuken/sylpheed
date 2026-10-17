/**
 * @file    gpu/pipeline/pso_recorder.h
 * @brief   Background precompile of the compiled-in pipeline list, and the
 *          capture that extends it.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include "gpu/pipeline/pipeline_state.h"

namespace sylpheed::gpu {

// Called for every PSO a draw binds (sanitized). 'builtOnRenderThread' means
// the draw compiled it synchronously because the precache list lacks it:
// logged once per pipeline, and captured under sylpheed_pso_capture.
void RecordPipelineState(const PipelineState &state, u32 renderPassId,
                         bool builtOnRenderThread);

// Starts compiling the compiled-in list on background threads, each entry
// once its shaders and vertex declaration exist. Call once after the host
// device and main pipeline layout are ready.
void ReplayBootCache();

// Stops the background compiles (waiting for any in progress) and writes the
// capture. Call before the device is torn down.
void ShutdownPsoRecorder();

// Called right after creating a guest shader / vertex decl so a deferred replay
// entry waiting on that content hash can resolve and enqueue.
void OnShaderCreated(u64 microcodeHash);
void OnVertexDeclarationCreated(u64 elementHash);

} // namespace sylpheed::gpu
