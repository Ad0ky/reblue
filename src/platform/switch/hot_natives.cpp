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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "cpu/hot_calls.h"

// The in-place build (see REBLUE_HOT_HOOK below).
#ifndef REBLUE_HOT_IN_PLACE
#define REBLUE_HOT_IN_PLACE 0
#endif

// See the guest memory macros below.
#ifndef REBLUE_HOT_VOLATILE
#define REBLUE_HOT_VOLATILE 0
#endif

REXCVAR_DEFINE_BOOL(bd_cpu_natives, true, "Switch",
                    "Run the localized copies of the hottest game functions "
                    "(src/platform/switch/hot/localized.txt) instead of the "
                    "recompiled originals. Read at start.");
REXCVAR_DEFINE_STRING(bd_cpu_natives_off, "", "Switch",
                      "Localized copies to run as the recompiled originals: "
                      "comma-separated function names, batches (first ... "
                      "fifth, as in localized.txt) or batch:first-last "
                      "positions (fifth:0-199). Read at start.");
REXCVAR_DEFINE_BOOL(bd_cpu_natives_trace, false, "Switch",
                    "Write each localized copy's first call to the log "
                    "(name, batch:position), to find which copies ran before "
                    "a crash. Read at start.");
REXCVAR_DEFINE_BOOL(bd_cpu_flush_skip, true, "Switch",
                    "In the localized copies, run short vector float "
                    "sequences without switching the FPU to flush-to-zero "
                    "and back, when no value in them is denormal (the result "
                    "is then the same bit for bit); any denormal sends that "
                    "operation through flush-to-zero as before. Read at "
                    "start.");

namespace {

// See platform/switch/hot_flush_skip.h. Set before the first copy runs (every copy is reached
// through NativesOn or NativeOn, which calls it).
bool g_flush_skip = false;

bool NativesOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(bd_cpu_natives);
#if defined(__aarch64__)
    g_flush_skip = REXCVAR_GET(bd_cpu_flush_skip);
#endif
    std::fprintf(stderr, "[hot] localized copies: %s (bd_cpu_natives), guest "
                 "memory %s, flush-mode skip %s (bd_cpu_flush_skip)\n",
                 REBLUE_HOT_IN_PLACE ? "in place (the build's form; bd_cpu_natives and bd_cpu_natives_off do "
                                       "not apply)" : (v ? "on" : "off"),
                 REBLUE_HOT_VOLATILE ? "volatile" : "plain", g_flush_skip ? "on" : "off");
    return v;
  }();
  return on;
}

// bd_cpu_natives_off: copies run as the recompiled originals instead, by
// name, by batch ("fifth") or by position in a batch ("fifth:0-199"), so a
// copy that misbehaves can be found by halving without a new build.
struct OffRule {
  std::string name;  // a function, or a batch with first..last
  std::string batch;
  int first = 0, last = -1;
};

const std::vector<OffRule> &OffRules() {
  static const std::vector<OffRule> rules = [] {
    std::vector<OffRule> out;
    const std::string spec = REXCVAR_GET(bd_cpu_natives_off);
    size_t at = 0;
    while (at <= spec.size()) {
      size_t end = spec.find(',', at);
      if (end == std::string::npos)
        end = spec.size();
      std::string tok = spec.substr(at, end - at);
      while (!tok.empty() && tok.front() == ' ')
        tok.erase(tok.begin());
      while (!tok.empty() && tok.back() == ' ')
        tok.pop_back();
      if (!tok.empty()) {
        OffRule r;
        const size_t colon = tok.find(':');
        if (colon != std::string::npos) {
          r.batch = tok.substr(0, colon);
          const std::string range = tok.substr(colon + 1);
          const size_t dash = range.find('-');
          r.first = std::atoi(range.substr(0, dash).c_str());
          r.last = dash == std::string::npos
                       ? r.first
                       : std::atoi(range.substr(dash + 1).c_str());
        } else {
          r.name = tok; // a function or a whole batch
        }
        out.push_back(r);
      }
      at = end + 1;
    }
    if (!out.empty())
      std::fprintf(stderr, "[hot] bd_cpu_natives_off = \"%s\"\n",
                   spec.c_str());
    return out;
  }();
  return rules;
}

bool NativeDecide(const char *name, const char *batch, int index) {
  if (!NativesOn())
    return false;
  for (const OffRule &r : OffRules()) {
    if (!r.name.empty() && (r.name == name || r.name == batch))
      return false;
    if (!r.batch.empty() && r.batch == batch && index >= r.first &&
        index <= r.last)
      return false;
  }
  return true;
}

// Called once per copy, on its first call.
bool NativeOn(const char *name, const char *batch, int index) {
  const bool on = NativeDecide(name, batch, index);
  static const bool trace = REXCVAR_GET(bd_cpu_natives_trace);
  if (trace) {
    std::fprintf(stderr, "[hot] first call: %s %s:%d%s\n", name, batch, index,
                 on ? "" : " (off)");
    std::fflush(stderr);
  }
  return on;
}

} // namespace

// Guest memory without volatile: the copies keep the recompiled code's order
// of guest loads and stores within straight-line code and calls, and a
// compiler barrier heads every loop, so memory another thread writes is read
// again on each iteration. Compiled with -fno-strict-aliasing (CMake), since a
// guest address may be read with another width than it was written with.
// REBLUE_HOT_VOLATILE 1: every copy keeps the recompiled code's volatile
// guest accesses (registers stay localized), a diagnostic build.
//
// The batches marked volatile in localized.txt always do, through these:
// with plain accesses the fifth batch corrupted data at startup (the shaders
// arrived corrupted, the title menu was wrong, a save would not load), and
// with volatile ones it ran the whole test route (2026-11-04), while its zlib
// copy matches the original on a real deflate stream in a host test.
#define REBLUE_VLOAD_U8(x) (*(volatile u8 *)(base + (u32)(x)))
#define REBLUE_VLOAD_U16(x) __builtin_bswap16(*(volatile u16 *)(base + (u32)(x)))
#define REBLUE_VLOAD_U32(x) __builtin_bswap32(*(volatile u32 *)(base + (u32)(x)))
#define REBLUE_VLOAD_U64(x) __builtin_bswap64(*(volatile u64 *)(base + (u32)(x)))
#define REBLUE_VSTORE_U8(x, y) (*(volatile u8 *)(base + (u32)(x)) = (y))
#define REBLUE_VSTORE_U16(x, y) \
  (*(volatile u16 *)(base + (u32)(x)) = __builtin_bswap16(y))
#define REBLUE_VSTORE_U32(x, y) \
  (*(volatile u32 *)(base + (u32)(x)) = __builtin_bswap32(y))
#define REBLUE_VSTORE_U64(x, y) \
  (*(volatile u64 *)(base + (u32)(x)) = __builtin_bswap64(y))
#if !REBLUE_HOT_VOLATILE
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
#endif
// stvlx / stvrx (localize.py merges their byte loops into these): the bytes
// of the vector from ea up to the end of its 16-byte block, and the bytes
// before ea back to the start of its block, in guest (big-endian) order.
// The vector is held reversed (u8[15] is the guest's first byte).
#define REBLUE_STVLX(ea, v) reblue_stvlx(base, (ea), (v))
#define REBLUE_STVRX(ea, v) reblue_stvrx(base, (ea), (v))
static inline void reblue_stvlx(u8 *base, u32 ea, const u8 *v) {
  u8 be[16];
  for (int i = 0; i < 16; ++i)
    be[i] = v[15 - i];
  std::memcpy(base + ea, be, 16 - (ea & 0xF));
}
static inline void reblue_stvrx(u8 *base, u32 ea, const u8 *v) {
  const u32 n = ea & 0xF;
  if (!n)
    return;
  u8 be[16];
  for (int i = 0; i < 16; ++i)
    be[i] = v[15 - i];
  std::memcpy(base + (ea - n), be + 16 - n, n);
}
#define REBLUE_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#define REBLUE_FP_FENCE() __asm__ __volatile__("" ::: "memory")

// Flush-mode skip (bd_cpu_flush_skip): the checked vector float operations
// localize.py writes into short vector sequences.
#include "platform/switch/hot_flush_skip.h"

// A function no hook replaces: this is its hook.
// In-place build (REBLUE_HOT_IN_PLACE, run-switch-build.bat -InPlace; the renut-nx base's guide,
// docs/hand-and-machine-optimising-guide.md, "hooked vs in place"): each copy IS the game function, with no on/off
// check in front of it, so the original is left unreferenced and the link drops it. The keys
// (bd_cpu_natives, bd_cpu_natives_off) do not apply; the hooked form (the default) keeps them for finding a copy
// that misbehaves. Each form needs its own PGO profile.
#if REBLUE_HOT_IN_PLACE
#define REBLUE_HOT_ON(name, batch, index) true
#define REBLUE_HOT_ORIGINAL(name) n_##name
#else
#define REBLUE_HOT_ON(name, batch, index) NativeOn(#name, batch, index)
#define REBLUE_HOT_ORIGINAL(name) __imp__##name
#endif
#define REBLUE_HOT_HOOK(name, batch, index)                                    \
  extern "C" REX_FUNC(name) {                                                  \
    static const bool on = REBLUE_HOT_ON(name, batch, index);                  \
    if (on) [[likely]]                                                         \
      n_##name(ctx, base);                                                     \
    else                                                                       \
      REBLUE_HOT_ORIGINAL(name)(ctx, base);                                    \
  }
// The same, counting and timing the copy's calls; a line every 10 s:
// calls per second, average and longest call, share of one core.
struct HotTimer {
  std::atomic<u64> calls{0}, ticks{0}, maxTicks{0}, windowStart{0};
};
static void HotTimeReport(HotTimer &t, const char *name, u64 now) {
  u64 start = t.windowStart.load(std::memory_order_relaxed);
  if (start == 0) {
    t.windowStart.compare_exchange_strong(start, now);
    return;
  }
  if (now - start < 10ull * 19200000ull)
    return;
  if (!t.windowStart.compare_exchange_strong(start, now))
    return;
  const u64 calls = t.calls.exchange(0), ticks = t.ticks.exchange(0),
            longest = t.maxTicks.exchange(0);
  const double seconds = double(now - start) / 19200000.0;
  std::fprintf(stderr,
               "[hot-time] %s: %.1f calls/s, %.1f us a call (longest %.1f), "
               "%.0f%% of a core\n",
               name, calls / seconds, calls ? ticks / 19.2 / calls : 0.0,
               longest / 19.2, 100.0 * ticks / 19200000.0 / seconds);
}
#if defined(__aarch64__)
// The system counter (armGetSystemTick), 19.2 MHz on the Switch.
static inline u64 HotTicks() {
  u64 t;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(t));
  return t;
}
#else
static inline u64 HotTicks() { return 0; }
#endif
#define REBLUE_HOT_TICKS() HotTicks()
#define REBLUE_HOT_HOOK_TIMED(name, batch, index)                              \
  extern "C" REX_FUNC(name) {                                                  \
    static const bool on = REBLUE_HOT_ON(name, batch, index);                  \
    static HotTimer timer;                                                     \
    const u64 t0 = REBLUE_HOT_TICKS();                                         \
    if (on) [[likely]]                                                         \
      n_##name(ctx, base);                                                     \
    else                                                                       \
      REBLUE_HOT_ORIGINAL(name)(ctx, base);                                    \
    const u64 t1 = REBLUE_HOT_TICKS();                                         \
    const u64 d = t1 - t0;                                                     \
    timer.calls.fetch_add(1, std::memory_order_relaxed);                       \
    timer.ticks.fetch_add(d, std::memory_order_relaxed);                       \
    u64 m = timer.maxTicks.load(std::memory_order_relaxed);                    \
    while (d > m && !timer.maxTicks.compare_exchange_weak(m, d)) {             \
    }                                                                          \
    HotTimeReport(timer, #name, t1);                                           \
  }
// A function a hook in the sources replaces: what that hook calls.
#define REBLUE_HOT_CALLER(name)                                                \
  namespace bd::hot {                                                          \
  REX_FUNC(Call_##name) {                                                      \
    if (REBLUE_HOT_IN_PLACE || NativesOn()) [[likely]]                         \
      n_##name(ctx, base);                                                     \
    else                                                                       \
      REBLUE_HOT_ORIGINAL(name)(ctx, base);                                    \
  }                                                                            \
  }
// The same when the script left the function to the recompiled code.
#define REBLUE_HOT_CALLER_ORIGINAL(name)                                       \
  namespace bd::hot {                                                          \
  REX_FUNC(Call_##name) { __imp__##name(ctx, base); }                          \
  }

#include "hot/localized.inc"

// Reads bd_cpu_natives / bd_cpu_flush_skip and writes the "[hot]" line once the config is loaded (switch_tuning.cpp
// calls it at start). The hooked form would also do it at the first call of a copy; the in-place form never asks.
extern "C" void ReblueHotInit() { (void)NativesOn(); }
