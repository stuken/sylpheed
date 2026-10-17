// sylpheed - ReXGlue Recompiled Project

#include "platform/guest_abort_log.h"

#include <csignal>
#include <format>
#include <string>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "core/logging.h"
#if defined(_WIN32)
#include "core/windows_lean.h"
#endif

namespace sylpheed::platform {

namespace {

void OnAbort(int sig) {
  if (auto *ts = rex::runtime::ThreadState::Get()) {
    const auto *c = ts->context();
    SYLPHEED_CRITICAL("guest context at abort: lr={:08X} ctr={:08X} r1={:08X} "
                 "r3={:08X} r4={:08X} r5={:08X} r30={:08X} r31={:08X}",
                 u32(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32,
                 c->r5.u32, c->r30.u32, c->r31.u32);
  } else {
    SYLPHEED_CRITICAL("abort on a thread with no guest context");
  }
  rex::FlushLogging();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

} // namespace

#if defined(_WIN32)
extern "C" char __ImageBase;
#endif

// Runs after the SDK's handlers, so it only sees faults nothing recovered.
// The host pc is logged relative to the exe so it resolves offline with
// llvm-symbolizer --obj=sylpheed.exe 0x<offset>; it names the recompiled guest
// function that faulted.
bool OnException(rex::arch::Exception *ex, void *) {
  u64 image_base = 0;
#if defined(_WIN32)
  image_base = reinterpret_cast<u64>(&__ImageBase);
#endif
  const u64 pc = ex->pc();
  SYLPHEED_CRITICAL("host exception code={} pc=sylpheed.exe+0x{:X} (0x{:016X}) fault=0x{:016X}",
               static_cast<int>(ex->code()), pc - image_base, pc,
               ex->code() == rex::arch::Exception::Code::kAccessViolation
                   ? ex->fault_address()
                   : 0);
#if defined(_WIN32)
  // Return addresses on the faulting host stack that fall inside the exe,
  // innermost first: a heuristic backtrace for llvm-symbolizer.
  if (const auto *hc = ex->thread_context()) {
    const auto *nt = reinterpret_cast<const u8 *>(image_base) +
                     *reinterpret_cast<const u32 *>(image_base + 0x3C);
    const u32 image_size = *reinterpret_cast<const u32 *>(nt + 0x50);
    const auto *sp = reinterpret_cast<const u64 *>(hc->rsp);
    std::string trace;
    u32 found = 0;
    for (u32 i = 0; i < 1024 && found < 16; ++i) {
      const u64 v = sp[i];
      if (v > image_base && v < image_base + image_size) {
        trace += std::format(" 0x{:X}", v - image_base);
        ++found;
      }
    }
    SYLPHEED_CRITICAL("host stack (sylpheed.exe offsets):{}", trace);
  }
#endif
  if (auto *ts = rex::runtime::ThreadState::Get()) {
    const auto *c = ts->context();
    SYLPHEED_CRITICAL("guest context: lr={:08X} ctr={:08X} r1={:08X} r3={:08X} "
                 "r4={:08X} r5={:08X} r30={:08X} r31={:08X}",
                 u32(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32,
                 c->r5.u32, c->r30.u32, c->r31.u32);
  }
  rex::FlushLogging();
  return false;
}

#if defined(_WIN32)
// "module+0xoffset" for a host address, or "" when no module holds it.
std::string ModuleOffset(u64 address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module)
    return {};
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring name(path);
  if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
    name.erase(0, slash + 1);
  std::string narrow;
  for (wchar_t c : name)
    narrow += static_cast<char>(c);
  return std::format("{}+0x{:X}", narrow,
                     address - reinterpret_cast<u64>(module));
}

// Exceptions nothing handled that are not access violations (those reach
// OnException through the SDK): a host component raising, such as DXGI's
// facility 0x87A codes. Logs the faulting address and every return address on
// the stack that lands in a loaded module, then lets Windows terminate.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS *ep) {
  const auto *rec = ep ? ep->ExceptionRecord : nullptr;
  const auto *ctx = ep ? ep->ContextRecord : nullptr;
  if (!rec || !ctx)
    return EXCEPTION_CONTINUE_SEARCH;
  SYLPHEED_CRITICAL("unhandled exception 0x{:08X} at {} (thread {})",
                    u32(rec->ExceptionCode),
                    ModuleOffset(reinterpret_cast<u64>(rec->ExceptionAddress)),
                    GetCurrentThreadId());
  std::string trace;
  const auto *sp = reinterpret_cast<const u64 *>(ctx->Rsp);
  u32 found = 0;
  for (u32 i = 0; i < 4096 && found < 32; ++i) {
    if (IsBadReadPtr(sp + i, sizeof(u64)))
      break;
    if (auto where = ModuleOffset(sp[i]); !where.empty()) {
      trace += " " + where;
      ++found;
    }
  }
  SYLPHEED_CRITICAL("stack:{}", trace);
  if (auto *ts = rex::runtime::ThreadState::Get()) {
    const auto *c = ts->context();
    SYLPHEED_CRITICAL("guest context: lr={:08X} r1={:08X}", u32(c->lr), c->r1.u32);
  }
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void InstallGuestAbortLogger() {
  std::signal(SIGABRT, OnAbort);
  rex::arch::ExceptionHandler::Install(OnException, nullptr);
#if defined(_WIN32)
  SetUnhandledExceptionFilter(OnUnhandledException);
#endif
}

} // namespace sylpheed::platform
