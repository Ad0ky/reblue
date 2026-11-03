/**
 * @file    engine/anim_curve.cpp
 * @brief   The animation curve key search (sub_8272EB40) as native code.
 *
 * sub_8272EED0 evaluates an animation curve at a time: sub_8272EB40 finds the
 * key the time falls on or after, with a linear scan over the curve's keys,
 * and sub_8272EDC8 interpolates. In the slow field scenes the scan was ~13%
 * of the Main Thread: the recompiled loop converts each key's 16-bit time
 * through the guest stack (a store and a load per key) and checks the flush
 * mode on every iteration. This is the same scan with the same arithmetic
 * (the key time to float, the fused multiply-subtract, the same compares) and
 * leaves the same registers and guest memory behind.
 *
 * Its first 50000 calls run the recompiled function as well and compare
 * everything it leaves (the two result words, the registers it writes); any
 * difference and the native scan stays off for the session (stderr says so).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"

REXCVAR_DEFINE_BOOL(bd_native_anim_curve, true, "Switch",
                    "The animation curve key search (sub_8272EB40) as native "
                    "code with the same arithmetic; false runs the "
                    "recompiled function.");

REX_EXTERN(__imp__sub_8272EB40);

namespace {

// The search state the function writes: +0 the key pointer, +8 the time it
// searches for, +12 where the time fell (0 on a key, 1 before the first, 2
// between two, 3 past the last, 4 no curve).
constexpr u32 kStateVa = 0x827A7A88;     // lis r11,-32134; addi 31368
constexpr u32 kZeroVa = 0x820012AC;      // lis -32256, 4780 (float)
constexpr u32 kEpsilonVa = 0x820536E8;   // lis -32251, 14056 (double)

constexpr u32 kVerifyCalls = 50000;

void Native(PPCContext &ctx, u8 *base) {
  (void)base;
  const u32 curve = ctx.r3.u32;
  ctx.r11.s64 = -2105933824; // 0x827A0000
  if (bd::mem::load<u8>(curve) == 0) {
    ctx.r11.s64 = ctx.r11.s64 + 31368;
    bd::mem::store<u32>(kStateVa, 0);
    ctx.r10.s64 = 4;
    bd::mem::store<u32>(kStateVa + 12, 4);
    return;
  }
  ctx.r10.u64 = bd::mem::load<u32>(curve + 4);
  ctx.r9.s64 = 0;
  ctx.r8.u64 = bd::mem::load<u32>(curve + 8);
  ctx.r6.s64 = ctx.r11.s64 + 31368;
  u32 where = 3;
  if (ctx.r10.s32 > 0) {
    ctx.r7.s64 = -2113601536; // 0x82050000
    ctx.fpscr.disableFlushMode();
    const u32 scale_bits = bd::mem::load<u32>(curve + 12);
    const u32 time_bits = bd::mem::load<u32>(kStateVa + 8);
    const u64 eps_bits = bd::mem::load<u64>(kEpsilonVa);
    const u32 zero_bits = bd::mem::load<u32>(kZeroVa);
    float scale_f, time_f, zero_f;
    double eps;
    std::memcpy(&scale_f, &scale_bits, 4);
    std::memcpy(&time_f, &time_bits, 4);
    std::memcpy(&zero_f, &zero_bits, 4);
    std::memcpy(&eps, &eps_bits, 8);
    const double f10 = double(scale_f), f13 = double(time_f);
    const double f12 = double(zero_f);
    ctx.f10.f64 = f10;
    ctx.f13.f64 = f13;
    ctx.f11.f64 = eps;
    ctx.f12.f64 = f12;
    ctx.r11.s64 = -2113929216; // 0x82000000

    const i32 count = ctx.r10.s32;
    u32 key = ctx.r8.u32;
    i32 i = 0;
    double f0 = 0.0;
    i16 key_time = 0;
    bool found = false;
    for (;;) {
      key_time = i16(bd::mem::load<u16>(key));
      f0 = double(float(double(i64(key_time))));
      f0 = double(float(-std::fma(f0, f10, -f13)));
      if (f0 < f12) {
        where = i != 0 ? 2 : 1;
        found = true;
        break;
      }
      f0 = std::fabs(f0);
      if (f0 < eps) {
        where = 0;
        found = true;
        break;
      }
      ++i;
      key += 8;
      if (!(i < count))
        break;
    }
    (void)found;
    ctx.f0.f64 = f0;
    ctx.r9.s64 = i;
    ctx.r8.u64 = key;
    // The recompiled loop leaves the last key's time in r11 until the result
    // replaces it, and stores it at r1-16 for the conversion.
    bd::mem::store<u64>(ctx.r1.u32 - 16, u64(i64(key_time)));
  }
  ctx.r11.s64 = where;
  bd::mem::store<u32>(ctx.r6.u32 + 0, ctx.r8.u32);
  bd::mem::store<u32>(ctx.r6.u32 + 12, ctx.r11.u32);
}

bool g_disabled = false;
u32 g_verified = 0;

bool Same(const PPCContext &a, const PPCContext &b) {
  const PPCRegister *ar[] = {&a.r6, &a.r7, &a.r8, &a.r9, &a.r10, &a.r11,
                             &a.f0, &a.f10, &a.f11, &a.f12, &a.f13};
  const PPCRegister *br[] = {&b.r6, &b.r7, &b.r8, &b.r9, &b.r10, &b.r11,
                             &b.f0, &b.f10, &b.f11, &b.f12, &b.f13};
  for (size_t i = 0; i < std::size(ar); ++i)
    if (ar[i]->u64 != br[i]->u64)
      return false;
  return true;
}

} // namespace

REX_HOOK_RAW(sub_8272EB40) {
  static const bool on = REXCVAR_GET(bd_native_anim_curve);
  if (!on || g_disabled) [[unlikely]] {
    __imp__sub_8272EB40(ctx, base);
    return;
  }
  if (g_verified < kVerifyCalls) [[unlikely]] {
    // The original first, on a copy; then the native on the real context.
    PPCContext ref = ctx;
    const u32 sp = ctx.r1.u32;
    __imp__sub_8272EB40(ref, base);
    const u32 ref_ptr = bd::mem::load<u32>(kStateVa);
    const u32 ref_where = bd::mem::load<u32>(kStateVa + 12);
    const u64 ref_stack = bd::mem::load<u64>(sp - 16);
    Native(ctx, base);
    const u32 ptr = bd::mem::load<u32>(kStateVa);
    const u32 where = bd::mem::load<u32>(kStateVa + 12);
    const bool ok = Same(ref, ctx) && ptr == ref_ptr && where == ref_where;
    if (!ok) {
      g_disabled = true;
      std::fprintf(stderr,
                   "[anim-curve] native key search differs from the game's "
                   "(call %u: key %08X/%08X, result %u/%u): kept off\n",
                   g_verified, ptr, ref_ptr, where, ref_where);
      // Leave exactly what the original left.
      ctx = ref;
      bd::mem::store<u32>(kStateVa, ref_ptr);
      bd::mem::store<u32>(kStateVa + 12, ref_where);
      bd::mem::store<u64>(sp - 16, ref_stack);
      return;
    }
    if (++g_verified == kVerifyCalls)
      std::fprintf(stderr, "[anim-curve] native key search: %u calls "
                           "identical to the game's: on\n",
                   kVerifyCalls);
    return;
  }
  Native(ctx, base);
}
