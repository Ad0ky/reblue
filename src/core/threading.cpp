/**
 * @file    core/threading.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/threading.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/logging.h"

#include <rex/ppc.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#if defined(_WIN32)
#include "core/windows_lean.h"
#include <timeapi.h>
#elif defined(__SWITCH__)
#include <cstdio>
extern "C" {
// libnx runtime internals (the same exit the SDK's rex_app.cpp takes).
extern unsigned int __nx_applet_exit_mode;
[[noreturn]] void __libnx_exit(int rc);
}
#else
#include <sys/resource.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace {
std::once_flag g_timer_init;

#if defined(__x86_64__) || defined(_M_X64)
inline void CpuPause() { _mm_pause(); }
#else
inline void CpuPause() { std::this_thread::yield(); }
#endif
} // namespace

namespace bd {

void EnableHighResTimer() {
  std::call_once(g_timer_init, [] {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
  });
}

void DisableHighResTimer() {
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void DemoteThreadToBackground() {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__SWITCH__)
  // Horizon does not time-share between priorities: a lower one only runs
  // when every higher thread on its core waits, so a demoted worker could
  // starve. It keeps the priority it was created with.
#else
  // who=0 is the calling THREAD on Linux, not the process.
  ::setpriority(PRIO_PROCESS, 0, 10);
#endif
}

void TerminateProcessNow(int exit_code) {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(),
                     static_cast<unsigned int>(exit_code));
  // TerminateProcess can return before the caller halts, so spin and nothing
  // runs past here.
  for (;;) {
    CpuPause();
  }
#elif defined(__SWITCH__)
  // std::_Exit returns the NRO to hbloader with the guest, GPU and audio
  // threads still alive, and the Homebrew Menu then crashes. An application
  // ends through the applet exit (see the SDK's rex_app.cpp, OnClosing).
  (void)exit_code;
  std::fflush(nullptr);
  __nx_applet_exit_mode = 1;
  __libnx_exit(0);
#else
  std::_Exit(exit_code);
#endif
}

} // namespace bd

// PPC kernel bypass for Sleep.
u32 Sleep_hook(u32 ms) {
  bd::EnableHighResTimer();

  if (ms == 0) {
    std::this_thread::yield();
    return 0;
  }

  auto target =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(u32(ms));

  if (ms >= 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(u32(ms)) -
                                std::chrono::microseconds(1500));
  } else {
    std::this_thread::yield();
  }

  while (std::chrono::steady_clock::now() < target)
    CpuPause();

  return 0;
}
REX_HOOK(rex_Sleep, Sleep_hook);

// PPC kernel bypass for NtSuspendThread.
u32 NtSuspendThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Suspend();
  return 0;
}
REX_HOOK(rex_NtSuspendThread, NtSuspendThread_hook);

// PPC kernel bypass for ResumeThread.
u32 ResumeThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Resume();
  return 0;
}
REX_HOOK(rex_ResumeThread, ResumeThread_hook);
