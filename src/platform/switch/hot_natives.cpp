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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "cpu/hot_calls.h"

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

// A function no hook replaces: this is its hook.
#define REBLUE_HOT_HOOK(name, batch, index)                                    \
  extern "C" REX_FUNC(name) {                                                  \
    static const bool on = NativeOn(#name, batch, index);                      \
    if (on) [[likely]]                                                         \
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
