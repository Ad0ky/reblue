/**
 * @file    platform/switch/hot_natives.cpp
 * @brief   Localized copies of the hottest recompiled functions (Switch).
 *
 * tools/switch/localize.py writes generated/hot/localized.inc from the
 * functions in hot/localized.txt: the same statements as the recompiled
 * function, with the context registers in C++ locals synced around calls and
 * returns, and guest memory accessed without volatile. See the script for the
 * rules. bd_cpu_natives = false runs the recompiled originals instead.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "reblue_pch.h"

#include <cstdio>

#include <rex/cvar.h>

#include "cpu/hot_calls.h"

REXCVAR_DEFINE_BOOL(bd_cpu_natives, true, "Switch",
                    "Run the localized copies of the hottest game functions "
                    "(src/platform/switch/hot/localized.txt) instead of the "
                    "recompiled originals. Read at start.");

namespace {

bool NativesOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(bd_cpu_natives);
    std::fprintf(stderr, "[hot] localized copies: %s (bd_cpu_natives)\n",
                 v ? "on" : "off");
    return v;
  }();
  return on;
}

} // namespace

// Guest memory without volatile: the copies keep the recompiled code's order
// of guest loads and stores within straight-line code and calls, and a
// compiler barrier heads every loop, so memory another thread writes is read
// again on each iteration. Compiled with -fno-strict-aliasing (CMake), since a
// guest address may be read with another width than it was written with.
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(u8 *)(base + (u32)(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16 *)(base + (u32)(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32 *)(base + (u32)(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64 *)(base + (u32)(x)))
#define REX_STORE_U8(x, y) (*(u8 *)(base + (u32)(x)) = (y))
#define REX_STORE_U16(x, y) (*(u16 *)(base + (u32)(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32 *)(base + (u32)(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64 *)(base + (u32)(x)) = __builtin_bswap64(y))
#define REBLUE_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#define REBLUE_FP_FENCE() __asm__ __volatile__("" ::: "memory")

// A function no hook replaces: this is its hook.
#define REBLUE_HOT_HOOK(name)                                                  \
  extern "C" REX_FUNC(name) {                                                  \
    if (NativesOn()) [[likely]]                                                \
      n_##name(ctx, base);                                                     \
    else                                                                       \
      __imp__##name(ctx, base);                                                \
  }
// A function a hook in the sources replaces: what that hook calls.
#define REBLUE_HOT_CALLER(name)                                                \
  namespace bd::hot {                                                          \
  REX_FUNC(Call_##name) {                                                      \
    if (NativesOn()) [[likely]]                                                \
      n_##name(ctx, base);                                                     \
    else                                                                       \
      __imp__##name(ctx, base);                                                \
  }                                                                            \
  }
// The same when the script left the function to the recompiled code.
#define REBLUE_HOT_CALLER_ORIGINAL(name)                                       \
  namespace bd::hot {                                                          \
  REX_FUNC(Call_##name) { __imp__##name(ctx, base); }                          \
  }

#include "hot/localized.inc"
