/**
 * @file    gpu/dred.h
 * @brief   D3D12 Device Removed Extended Data: auto-breadcrumbs + page fault
 *          allocation reporting, dumped to the log when the device is lost.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

namespace sylpheed::gpu {

// Every entry point is a no-op on the Vulkan backend.

// Arms DRED for devices created afterwards, so it must run before the render
// interface creates one. The settings object is process-wide. Returns whether
// breadcrumbs and page fault reporting are armed.
bool EnableDred();

// Labels the direct queue and the per-slot command lists, the only identity a
// breadcrumb node carries. Call once the frame ring's lists exist. Returns
// whether the direct queue can write breadcrumbs at all.
bool PrepareDredCommandObjects();

// Walks the removed device's breadcrumb and page fault output into the log:
// which list was executing, the op it stopped on, and the allocations live or
// recently freed at the faulting VA.
void LogDredReport(const char *context);

// sylpheed_d3d12_debug: turns on the D3D12 debug layer (needs Graphics Tools) for
// devices created afterwards, so call it next to EnableDred. Returns whether
// the layer is on.
bool EnableDebugLayer();

// Routes debug layer messages into the sylpheed log once the device exists, so a
// validation error shows up beside the guest call that caused it.
void InstallDebugMessageLogger();

// sylpheed_d3d12_no_shader_cache: stops this process reading or writing the
// driver's and the runtime's shader caches (Windows developer mode only), so
// every pipeline compiles from scratch as on a first run. For measuring
// pipeline compile cost; the caches on disk are left alone.
void ApplyShaderCacheControl();

} // namespace sylpheed::gpu
