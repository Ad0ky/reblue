/**
 * @file    platform/switch/crash_handler_switch.cpp
 * @brief   Nintendo Switch version of crash_handler.h.
 *
 *          Horizon has no signals, no dladdr and no backtrace(): faults reach
 *          the process through libnx's __libnx_exception_handler, which the
 *          SDK owns (exception_handler_switch.cpp) and dispatches to the
 *          rex::arch::ExceptionHandler chain. This handler is chained after
 *          the SDK's guest-memory handler, so it only sees faults the runtime
 *          did not resolve. It logs the fault, the registers and the code
 *          addresses on the stack as offsets from the NRO's base (_start sits
 *          at address 0 of reblue.elf), so they resolve with
 *            aarch64-none-elf-addr2line -fCe out/switch/reblue.elf <offset>
 * @license BSD 3-Clause, see LICENSE
 */
#include "platform/crash_handler.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/types.h>
#include <rex/version.h>

#include "core/logging.h"
#include "platform/fatal_dialog.h"

// libnx's crt0 entry point, the first byte of the NRO's code.
extern "C" char _start[];

namespace bd::platform {
namespace {

constexpr u64 kGuestAddressSpaceSize = 0x100000000ull;
// Upper bound of the NRO image (code plus data, the recompiled game included).
constexpr u64 kHostImageSpan = 0x20000000ull;

std::atomic_flag s_reporting = ATOMIC_FLAG_INIT;

u64 ImageBase() { return reinterpret_cast<u64>(&_start[0]); }

bool InImage(u64 a) {
  const u64 base = ImageBase();
  return a >= base && a - base < kHostImageSpan;
}

const char *ExceptionCodeName(rex::arch::Exception::Code code) {
  using Code = rex::arch::Exception::Code;
  switch (code) {
  case Code::kAccessViolation:
    return "access violation";
  case Code::kIllegalInstruction:
    return "illegal instruction";
  default:
    return "exception";
  }
}

void LogRegisters(const rex::arch::HostThreadContext *ctx) {
  if (!ctx)
    return;
#if REX_ARCH_ARM64
  BD_CRITICAL("    pc = {:#018x} (reblue.elf+{:#x})   sp = {:#018x}", ctx->pc,
              InImage(ctx->pc) ? ctx->pc - ImageBase() : 0, ctx->sp);
  for (int i = 0; i < 31; ++i) {
    const u64 v = ctx->x[i];
    if (InImage(v))
      BD_CRITICAL("    x{:<2} = {:#018x}  (reblue.elf+{:#x})", i, v, v - ImageBase());
    else
      BD_CRITICAL("    x{:<2} = {:#018x}", i, v);
  }
#endif
}

// Return addresses into the image found on the stack: the closest thing to a
// backtrace without unwind tables.
void LogStackCodeAddresses(const rex::arch::HostThreadContext *ctx) {
#if REX_ARCH_ARM64
  if (!ctx || !ctx->sp)
    return;
  const u64 sp = ctx->sp;
  BD_CRITICAL("stack code addresses (sp {:#018x}):", sp);
  u32 found = 0;
  for (u64 p = sp; p < sp + 0x2000; p += sizeof(u64)) {
    u64 v = 0;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof(v));
    if (!InImage(v))
      continue;
    BD_CRITICAL("    [sp+{:#05x}] reblue.elf+{:#x}", p - sp, v - ImageBase());
    if (++found >= 24)
      break;
  }
  if (!found)
    BD_CRITICAL("    (no image addresses on the stack)");
#else
  (void)ctx;
#endif
}

bool CrashHandler(rex::arch::Exception *ex, void * /*data*/) {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    return false;

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("image base: {:#018x}", ImageBase());
  BD_CRITICAL("exception: {} @ host pc {:#018x}", ExceptionCodeName(ex->code()),
              ex->pc());
  if (InImage(ex->pc()))
    BD_CRITICAL("faulting offset: reblue.elf+{:#x}", ex->pc() - ImageBase());

  if (ex->code() == rex::arch::Exception::Code::kAccessViolation) {
    const u64 fa = ex->fault_address();
    BD_CRITICAL("fault address: {:#018x}", fa);
    auto *rt = rex::Runtime::instance();
    u8 *membase = rt ? rt->virtual_membase() : nullptr;
    const u64 base = reinterpret_cast<u64>(membase);
    if (membase && fa >= base && fa < base + kGuestAddressSpaceSize)
      BD_CRITICAL("guest fault VA: {:#010x}", static_cast<u32>(fa - base));
  }

  BD_CRITICAL("registers:");
  LogRegisters(ex->thread_context());
  rex::FlushLogging();
  LogStackCodeAddresses(ex->thread_context());
  BD_CRITICAL("===================================================");
  rex::FlushLogging();

  const std::string where =
      InImage(ex->pc()) ? fmt::format("reblue.elf+{:#x}", ex->pc() - ImageBase())
                        : fmt::format("{:#018x}", ex->pc());
  ShowFatalError("re:Blue crashed",
                 fmt::format("re:Blue hit a fatal error and has to close.\n\n"
                             "{} at {}\n\nThe log in sdmc:/switch/reblue/logs/ "
                             "has the details.",
                             ExceptionCodeName(ex->code()), where));
  // Not handled: the SDK's handler ends the process.
  return false;
}

[[noreturn]] void TerminateHandler() {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    std::abort();

  std::string detail = "std::terminate with no active exception";
  if (std::exception_ptr active = std::current_exception()) {
    try {
      std::rethrow_exception(active);
    } catch (const std::exception &e) {
      detail = fmt::format("uncaught exception: {}", e.what());
    } catch (...) {
      detail = "uncaught exception of non-std type";
    }
  }
  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("{}", detail);
  BD_CRITICAL("===================================================");
  rex::FlushLogging();
  ShowFatalError("re:Blue crashed",
                 fmt::format("re:Blue hit a fatal error and has to close.\n\n{}", detail));
  std::abort();
}

} // namespace

void InstallCrashStackForThread() {
  // No sigaltstack on Horizon: libnx runs exception handlers on its own
  // exception stack (the SDK sizes it in runtime_switch.cpp).
}

void InstallTerminateHandler() { std::set_terminate(&TerminateHandler); }

void InstallCrashHandler() {
  rex::arch::ExceptionHandler::Install(&CrashHandler, nullptr);
  InstallTerminateHandler();
  BD_DEBUG("[crash] last-chance handler installed (image base {:#x})", ImageBase());
}

} // namespace bd::platform
