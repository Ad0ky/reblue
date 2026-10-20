/**
 * @file    core/threading.h
 * @brief   Native threading, sleep, and frame timing hooks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <thread>

namespace bd {

// Let 't' run on its own, never joined. std::thread::detach() everywhere but
// Nintendo Switch, where detach throws (libnx has no detached pthreads; see
// nfsmw-nx docs/platform-notes.md): there the thread object is parked in a
// list that is never destroyed, which is the same thing for a thread that
// lives until the process ends. Use this instead of .detach().
void DetachThread(std::thread &&t);

void EnableHighResTimer();
void DisableHighResTimer();

// Best effort. Never dies hard.
void DemoteThreadToBackground();

// TerminateProcess and spin. Never returns, runs no destructors (not a clean
// exit).
[[noreturn]] void TerminateProcessNow(int exit_code = 0);

} // namespace bd
