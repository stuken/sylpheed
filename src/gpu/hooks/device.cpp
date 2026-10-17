// sylpheed - ReXGlue Recompiled Project
//
// Device lifecycle hooks for the native renderer: Direct3D_CreateDevice builds
// the guest D3DDevice while plume owns the real swapchain, Clear and Present
// drive the host frame, and D3D__MakeSpace turns the guest command ring into a
// discard buffer. Modelled on reblue's gpu/hooks/device.cpp (Copyright (c) 2026
// Tom Clay, BSD 3-Clause), against Sylpheed's XDK 2.0.3215 layout
// (config/d3d.toml).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/runtime.h>
#include <rex/chrono/clock.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "core/shutdown.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"

void SylpheedRenderDocOnPresent(u32 frames); // platform/renderdoc_trigger.cpp

REXCVAR_DEFINE_INT32(sylpheed_sample_threads, 0, kCvarGroup,
                     "Log every guest thread's call chain every N vblanks "
                     "(0 = off), a poor man's profiler");

namespace sylpheed::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
void AdvanceGuestBufferFrame();
void LogDrawStats(u32 frame);
bool TraceFrameActive();
void MaybeScheduleTrace(u32 frame);
void MaybeTriggerRenderDocCapture(u32 presented);
} // namespace sylpheed::gpu

namespace {

// Nothing consumes the guest's PM4 ring: every guest D3D function still
// running unhooked writes its packets here and D3D__MakeSpace rewinds them.
// The limit leaves half the buffer as headroom for the largest single batch
// written between two limit checks.
constexpr u32 kDiscardRingSize = 4u << 20;
constexpr u32 kDiscardRingLimit = kDiscardRingSize / 2;
u32 g_discard_ring = 0;

// Command memory D3D__AllocCommandMemory hands out (inline vertex data,
// clear and resolve constants), bump-allocated and wrapped. The guest only
// writes it, so reuse after a wrap is harmless while nothing reads it back.
constexpr u32 kDiscardCommandSize = 8u << 20;
u32 g_discard_command = 0;
u32 g_discard_command_used = 0;

// xboxkrnl's VdGlobalDevice (ordinal 0x1BE), imported at this slot. The real
// D3DDevice_Initialize (0x824C7C10) publishes the device there and
// D3DResource_Destroy (0x824C1660) reads it back.
constexpr u32 kVdGlobalDeviceImport = 0x82000758;

// The device's two critical sections, which D3DDevice_Initialize sets up and
// the D3D code left running under the hooks still takes (0x824C14A0,
// 0x824CE508).
constexpr u32 kDeviceLockOffsets[] = {0x34EC, 0x3508};

REX_IMPORT(__imp__D3D__InitializeStateTables, InitializeStateTables,
           void(u32));
REX_IMPORT(__imp__RtlInitializeCriticalSection, InitializeCriticalSection,
           void(u32));
REX_IMPORT(__imp__D3DDevice_CreateSurface, CreateSurface,
           u32(u32, u32, u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetRenderTarget, SetRenderTarget,
           void(u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetDepthStencilSurface, SetDepthStencilSurface,
           void(u32, u32));

// D3DPRESENT_PARAMETERS, the fields the device's default surfaces need.
struct PresentParameters {
  be_u32 BackBufferWidth;        // +0x00
  be_u32 BackBufferHeight;       // +0x04
  be_u32 BackBufferFormat;       // +0x08
  be_u32 BackBufferCount;        // +0x0C
  be_u32 MultiSampleType;        // +0x10
  be_u32 MultiSampleQuality;     // +0x14
  be_u32 SwapEffect;             // +0x18
  be_u32 hDeviceWindow;          // +0x1C
  be_u32 Windowed;               // +0x20
  be_u32 EnableAutoDepthStencil; // +0x24
  be_u32 AutoDepthStencilFormat; // +0x28
};

// The real D3DDevice_Initialize binds a back buffer at RT0 (and a depth
// buffer when asked) before returning, and games read RT0 back through
// D3DDevice_GetRenderTarget. The guest's own CreateSurface/Set* keep its
// bookkeeping consistent; the device holds the creation references.
void BindDefaultSurfaces(u32 device_va, u32 params_va) {
  const auto *pp = sylpheed::mem::at<const PresentParameters>(params_va);
  if (!pp)
    return;
  const u32 width = pp->BackBufferWidth ? u32(pp->BackBufferWidth) : 1280u;
  const u32 height = pp->BackBufferHeight ? u32(pp->BackBufferHeight) : 720u;
  // D3D allocates these in EDRAM, which a multisampled 1280x720 pair can
  // overflow (2048 tiles); Sylpheed places its own targets, so the defaults
  // fall back to single-sampled ones.
  u32 msaa = pp->MultiSampleType;
  auto create = [&](u32 format) {
    u32 surface = CreateSurface(width, height, format, msaa, 0);
    if (!surface && msaa) {
      SYLPHEED_WARN("Direct3D_CreateDevice: {}x{} surface (format 0x{:08X}) "
                    "does not fit EDRAM at multisample type {}, retrying "
                    "single-sampled", width, height, format, msaa);
      msaa = 0;
      surface = CreateSurface(width, height, format, 0, 0);
    }
    return surface;
  };
  const u32 back = create(pp->BackBufferFormat);
  if (back)
    SetRenderTarget(device_va, 0, back);
  u32 depth = 0;
  if (pp->EnableAutoDepthStencil) {
    depth = create(pp->AutoDepthStencilFormat);
    if (depth)
      SetDepthStencilSurface(device_va, depth);
  }
  sylpheed::gpu::NoteDefaultSurfaces(back, depth);
  SYLPHEED_INFO("Direct3D_CreateDevice: {}x{} back buffer 0x{:08X} (format "
           "0x{:08X}), depth 0x{:08X} (format 0x{:08X}), multisample {} (asked {})",
           width, height, back, u32(pp->BackBufferFormat), depth,
           u32(pp->AutoDepthStencilFormat), msaa, u32(pp->MultiSampleType));
}

// D3DVBLANKDATA, what a vertical blank callback is handed.
// Frames presented so far (PresentFrame).
std::atomic<u32> g_presents{0};

struct VBlankData {
  be_u32 VBlank; // device+0x3B2C, blanks so far
  be_u32 Swap;   // device+0x3B38, swaps so far
  be_u32 Flags;
};

// The vertical blank. The real D3DDevice_Initialize hands the GPU interrupt
// to D3D's handler (0x824BFA58), whose vblank path counts the blank at
// device+0x3B2C and calls the callback D3DDevice_SetVerticalBlankCallback
// (0x824C29D8) left at +0x3B28. Sylpheed's render thread (0x822F30C0) waits
// on an event that callback signals before every frame. With no SDK GPU
// nothing raises the interrupt, and the handler reads GPU registers that are
// not mapped, so the host counts the blank and calls the callback instead.
constexpr u32 kVBlankCallbackOffset = 0x3B28;
constexpr u32 kVBlankCountOffset = 0x3B2C;
constexpr u32 kSwapCountOffset = 0x3B38;
constexpr u32 kVBlankHz = 60;
rex::system::object_ref<rex::system::XHostThread> g_vblank_thread;

// GPU callbacks D3DDevice_InsertCallback (0x824C0DE0) asked for, as
// {function, context}. The GPU would raise its interrupt once the commands
// before the callback had run, and D3D's handler would call it; the host
// renderer has no such point, so the vblank thread runs them at its next tick.
// Sylpheed's movie player (0x824FAFF0) frees each decoded frame this way.
std::mutex g_callback_mutex;
std::vector<std::pair<u32, u32>> g_pending_callbacks;

void RunPendingCallbacks(rex::system::KernelState *ks) {
  std::vector<std::pair<u32, u32>> pending;
  {
    std::lock_guard lock(g_callback_mutex);
    pending.swap(g_pending_callbacks);
  }
  auto *thread = rex::system::XThread::GetCurrentThread();
  for (const auto &[fn, context] : pending) {
    u64 args[] = {context};
    ks->function_dispatcher()->ExecuteInterrupt(thread->thread_state(), fn, args,
                                                std::size(args));
  }
}

void VBlankTick(rex::system::KernelState *ks, u32 device_va, u32 data_va) {
  auto *count = sylpheed::mem::at<be_u32>(device_va + kVBlankCountOffset);
  const auto *callback =
      sylpheed::mem::at<const be_u32>(device_va + kVBlankCallbackOffset);
  const auto *swaps = sylpheed::mem::at<const be_u32>(device_va + kSwapCountOffset);
  auto *data = sylpheed::mem::at<VBlankData>(data_va);
  if (!count || !callback || !swaps || !data)
    return;
  *count = u32(*count) + 1;
  const u32 fn = *callback;
  if (u32(*count) % (kVBlankHz * 5) == 0)
    SYLPHEED_DEV_INFO("vblank {}: callback 0x{:08X}, {} frames presented",
                      u32(*count), fn, g_presents.load(std::memory_order_relaxed));
  if (!fn)
    return;
  data->VBlank = u32(*count);
  data->Swap = u32(*swaps);
  data->Flags = 0;
  auto *thread = rex::system::XThread::GetCurrentThread();
  u64 args[] = {data_va};
  ks->function_dispatcher()->ExecuteInterrupt(thread->thread_state(), fn, args,
                                              std::size(args));
}

// Logs where every guest thread is when the game stops presenting: the return
// address recompiled code leaves in LR, then the saved LRs up the stack.
void DumpGuestThreads(rex::system::KernelState *ks) {
  for (auto &thread : ks->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!thread->is_guest_thread() || !thread->thread_state())
      continue;
    const PPCContext *ctx = thread->thread_state()->context();
    if (!ctx)
      continue;
    std::string chain = std::format(" {:08X}", u32(ctx->lr));
    u32 sp = ctx->r1.u32;
    for (int i = 0; sp && i < 8; ++i) {
      const auto *link = sylpheed::mem::try_at<const be_u32>(sp);
      if (!link || !u32(*link))
        break;
      sp = u32(*link);
      if (const auto *saved = sylpheed::mem::try_at<const be_u32>(sp - 8))
        chain += std::format(" {:08X}", u32(*saved));
    }
    SYLPHEED_WARN("  thread {:X} '{}':{}", thread->thread_id(), thread->name(), chain);
  }
}

void StartVBlank(u32 device_va) {
  if (g_vblank_thread)
    return;
  auto *ks = REX_KERNEL_STATE();
  const u32 data_va = ks->memory()->SystemHeapAlloc(sizeof(VBlankData), 0x10);
  if (!data_va)
    return;
  g_vblank_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(ks, 128 * 1024, 0, [ks, device_va, data_va]() {
        const u64 interval = std::max<u64>(
            1, rex::chrono::Clock::guest_tick_frequency() / kVBlankHz);
        u64 last = rex::chrono::Clock::QueryGuestTickCount();
        // Presents stalled for this many blanks get a thread dump, once.
        constexpr u32 kStallBlanks = kVBlankHz * 3;
        u32 stall_blanks = 0, last_presents = 0;
        bool dumped = false;
        while (!sylpheed::IsShuttingDown()) {
          const u64 now = rex::chrono::Clock::QueryGuestTickCount();
          while (now - last >= interval) {
            last += interval;
            VBlankTick(ks, device_va, data_va);
            RunPendingCallbacks(ks);
            const u32 presents = g_presents.load(std::memory_order_relaxed);
            if (presents != last_presents) {
              last_presents = presents;
              stall_blanks = 0;
              dumped = false;
            } else if (presents && ++stall_blanks >= kStallBlanks && !dumped) {
              dumped = true;
              SYLPHEED_WARN("no frame presented for {} blanks after frame {}; "
                            "guest threads:", stall_blanks, presents);
              DumpGuestThreads(ks);
            }
            // sylpheed_sample_threads=N: every Nth blank, where each guest
            // thread is, for finding what a busy guest thread spends its time on.
            static u32 s_blank = 0;
            if (const i32 every = REXCVAR_GET(sylpheed_sample_threads);
                every > 0 && ++s_blank % u32(every) == 0) {
              SYLPHEED_INFO("[sample] guest threads at blank {}:", s_blank);
              DumpGuestThreads(ks);
            }
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  g_vblank_thread->set_name("Sylpheed VBlank");
  g_vblank_thread->Create();
}

u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 /*device_type*/,
                               u32 /*focus_window*/, u32 behavior_flags,
                               u32 present_params, mapped_u32 out_device) {
  auto *memory = REX_KERNEL_MEMORY();
  const u32 device_va =
      memory->SystemHeapAlloc(sylpheed::gpu::kD3DDeviceAllocSize, 0x80);
  if (!g_discard_ring)
    g_discard_ring = memory->SystemHeapAlloc(kDiscardRingSize, 0x1000);
  if (!g_discard_command)
    g_discard_command = memory->SystemHeapAlloc(kDiscardCommandSize, 0x1000);
  if (!device_va || !g_discard_ring || !g_discard_command) {
    SYLPHEED_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return 0x8007000E; // E_OUTOFMEMORY, as the guest returns
  }
  memory->Zero(device_va, sylpheed::gpu::kD3DDeviceAllocSize);

  auto *device = sylpheed::mem::at<sylpheed::gpu::D3DDevice>(device_va);
  device->m_pRing = g_discard_ring;
  device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  device->m_pRingLimit = g_discard_ring + kDiscardRingLimit;
  device->m_ReferenceCount = 1;
  device->behaviorFlags = behavior_flags;

  if (auto *global = sylpheed::mem::at<const be_u32>(kVdGlobalDeviceImport)) {
    if (auto *slot = sylpheed::mem::at<be_u32>(global->get()))
      *slot = device_va;
  }

  for (const u32 offset : kDeviceLockOffsets)
    InitializeCriticalSection(device_va + offset);

  // The guest's own seeding: copies both dispatch tables in, calls every Set
  // with its default so the register shadows start valid, and clears all
  // texture slots. Its packets land in the discard ring.
  InitializeStateTables(device_va);
  BindDefaultSurfaces(device_va, present_params);

  device->viewport.Width = 1280;
  device->viewport.Height = 720;
  device->viewport.MaxZ = 1.0f;

  StartVBlank(device_va);

  if (out_device)
    *out_device = device_va;
  SYLPHEED_INFO("Direct3D_CreateDevice: guest device at 0x{:08X}", device_va);
  return 0;
}

// The Xenon ABI reserves a GPR slot for every float argument, so Z in f1 also
// consumes r8 while SDK marshaling numbers integer args without skipping it.
// The placeholder absorbs r8 so Stencil reads r9 (0x824C3CB0 passes r9 on).
u32 D3DDevice_Clear_hook(u32 /*device*/, u32 count, u32 rects, u32 flags,
                         u32 color, f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  if (sylpheed::gpu::TraceFrameActive())
    SYLPHEED_INFO("[trace] Clear flags=0x{:X} color=0x{:08X} z={}", flags, color, z);
  // D3DRECT {x1,y1,x2,y2}. Rects aren't modeled: the whole target clears.
  if (count && rects) {
    if (const auto *r = sylpheed::mem::at<const be_i32>(rects))
      SYLPHEED_WARN_FIRST(4, "Clear: {} rect(s) ignored, first {},{}-{},{} "
                     "flags=0x{:X}", count, i32(r[0]), i32(r[1]), i32(r[2]),
                     i32(r[3]), flags);
  }
  sylpheed::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  return 0;
}

// (device, flags, D3DRECT*, D3DVECTOR4* color as r,g,b,a, Z in f1, stencil
// in r8): what Clear calls once per rect, and what Sylpheed's frame
// (0x822CD4E8) calls directly for every target it clears. Z takes the r7
// slot, as in Clear.
u32 D3DDevice_ClearF_hook(u32 /*device*/, u32 flags, u32 rect, u32 color_va,
                          f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  if (sylpheed::gpu::TraceFrameActive())
    SYLPHEED_INFO("[trace] ClearF flags=0x{:X} z={}", flags, z);
  if (rect) {
    if (const auto *r = sylpheed::mem::at<const be_i32>(rect))
      SYLPHEED_WARN_FIRST(4, "ClearF: rect {},{}-{},{} ignored, flags=0x{:X}",
                          i32(r[0]), i32(r[1]), i32(r[2]), i32(r[3]), flags);
  }
  plume::RenderColor color(0.0f, 0.0f, 0.0f, 0.0f);
  if (const auto *c = sylpheed::mem::at<const be_f32>(color_va); c && color_va)
    color = plume::RenderColor(f32(c[0]), f32(c[1]), f32(c[2]), f32(c[3]));
  sylpheed::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  return 0;
}

// Plume owns the swapchain, so the host presents directly. front_buffer is
// the texture the game resolved its frame into, or null for the bound target.

void PresentFrame(u32 device_va, const char *entry,
                  sylpheed::gpu::GuestTexture *front_buffer) {
  // The swap count the vblank callback reports. The real Swap advances it
  // (0x824CF368); Sylpheed paces its frames on it and stalls two frames in
  // when it stays put.
  if (auto *swaps = sylpheed::mem::at<be_u32>(device_va + kSwapCountOffset))
    *swaps = u32(*swaps) + 1;
  const u32 s_presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s_presents == 1)
    SYLPHEED_INFO("{}: first frame", entry);
  if (s_presents == 60 || (s_presents % 600) == 0)
    sylpheed::gpu::LogDrawStats(s_presents);
  sylpheed::gpu::Video::Present(front_buffer);
  sylpheed::gpu::MaybeTriggerRenderDocCapture(s_presents);
  SylpheedRenderDocOnPresent(0);
  sylpheed::gpu::AdvanceGuestBufferFrame();
  sylpheed::gpu::MaybeScheduleTrace(s_presents);
}

// The XDK's convenience entry: resolves the back buffer to the front buffer
// and swaps (0x824CFB30, which calls D3DDevice_Swap itself).
u32 D3DDevice_Present_hook(u32 device) {
  PresentFrame(device, "D3DDevice_Present", nullptr);
  return 0;
}

// (device, pFrontBuffer, scaler params). Sylpheed's main loop (0x822F3238)
// resolves the frame itself and swaps directly, so the resolved front buffer
// is what the frame shows, as in reblue.
u32 D3DDevice_Swap_hook(u32 device, u32 front_buffer_va, u32 /*params*/) {
  PresentFrame(device, "D3DDevice_Swap",
               front_buffer_va ? sylpheed::gpu::FindResolveTarget(front_buffer_va)
                               : nullptr);
  return 0;
}

// (device, type, pCallback, Context): queued for the vblank thread, in place
// of the packets that would have the GPU raise it.
u32 D3DDevice_InsertCallback_hook(u32 /*device*/, u32 /*type*/, u32 callback,
                                  u32 context) {
  if (callback) {
    std::lock_guard lock(g_callback_mutex);
    g_pending_callbacks.emplace_back(callback, context);
  }
  return 0;
}

// (device). Closes the command stream D3D__BeginCallbackStream (0x824CEE50)
// opened. The guest would replay it by submitting the stream to the GPU and
// waiting on its read pointer (0x824CE758), and with no GPU the wait never
// ends. Nothing is left to replay: draws inside the stream reached the host
// through the draw hooks, and each GPU-time callback already ran when its
// packets were recorded (D3D__StreamPackets in draw.cpp). Undo what the
// replay would have left in the device and drop the stream.
u32 D3D__EndCallbackStream_hook(rex::MappedPtr<sylpheed::gpu::D3DDevice> device) {
  if (!device)
    return 0;
  const u32 va = device.guest_address();
  if (auto *flags = sylpheed::mem::at<u8>(va + 0x2A38))
    *flags = u8(*flags & ~0x04u);
  if (auto *recording = sylpheed::mem::at<be_u32>(va + 0x32A0)) {
    recording[0] = 0; // +0x32A0 stream base while recording
    recording[1] = 0; // +0x32A4
  }
  return 0;
}

// Callers compare the ring pointer against the limit and call this when a
// batch would cross it, then write from the returned pointer.
u32 D3D__MakeSpace_hook(rex::MappedPtr<sylpheed::gpu::D3DDevice> device) {
  if (device) {
    device->m_pRing = g_discard_ring;
    device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  }
  return g_discard_ring;
}

// The real allocator (0x824BFEC0) carves space out of the next ring segment,
// and once the segments run dry the swap that follows hands VdSwap a null
// buffer. With no segments to
// manage, hand out discard memory with the requested alignment instead.
u32 D3D__AllocCommandMemory_hook(u32 /*device*/, u32 dwords, u32 align) {
  const u32 bytes = dwords * 4;
  const u32 alignment = align ? align : 4;
  if (bytes > kDiscardCommandSize / 2) {
    SYLPHEED_ERROR("D3D__AllocCommandMemory: {} bytes exceeds the discard buffer",
              bytes);
    return 0;
  }
  u32 offset = (g_discard_command_used + alignment - 1) & ~(alignment - 1);
  if (offset + bytes > kDiscardCommandSize)
    offset = 0;
  g_discard_command_used = offset + bytes;
  return g_discard_command + offset;
}

} // namespace

namespace sylpheed::gpu {

// Frames the guest has presented, for tools that script against frame
// numbers (platform/input_script.cpp).
u32 PresentCount() { return g_presents.load(std::memory_order_relaxed); }

} // namespace sylpheed::gpu

REX_HOOK(D3D__AllocCommandMemory, D3D__AllocCommandMemory_hook);
REX_HOOK(Direct3D_CreateDevice, Direct3D_CreateDevice_hook);
REX_HOOK(D3DDevice_Clear, D3DDevice_Clear_hook);
REX_HOOK(D3DDevice_ClearF, D3DDevice_ClearF_hook);
REX_HOOK(D3DDevice_Present, D3DDevice_Present_hook);
REX_HOOK(D3DDevice_Swap, D3DDevice_Swap_hook);
REX_HOOK(D3D__MakeSpace, D3D__MakeSpace_hook);
REX_HOOK(D3DDevice_InsertCallback, D3DDevice_InsertCallback_hook);
REX_HOOK(D3D__EndCallbackStream, D3D__EndCallbackStream_hook);
