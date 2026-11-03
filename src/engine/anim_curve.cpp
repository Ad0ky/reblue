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
#include "cpu/hot_calls.h"

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

// Guest memory straight from the base pointer, as the recompiled code reads
// it (bd::mem goes through the kernel's memory object on every access).
inline u8 Ld8(const u8 *base, u32 va) { return base[va]; }
inline u16 Ld16(const u8 *base, u32 va) {
  u16 v;
  std::memcpy(&v, base + va, 2);
  return __builtin_bswap16(v);
}
inline u32 Ld32(const u8 *base, u32 va) {
  u32 v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
inline u64 Ld64(const u8 *base, u32 va) {
  u64 v;
  std::memcpy(&v, base + va, 8);
  return __builtin_bswap64(v);
}
inline void St32(u8 *base, u32 va, u32 x) {
  x = __builtin_bswap32(x);
  std::memcpy(base + va, &x, 4);
}
inline void St64(u8 *base, u32 va, u64 x) {
  x = __builtin_bswap64(x);
  std::memcpy(base + va, &x, 8);
}

void Native(PPCContext &ctx, u8 *base) {
  const u32 curve = ctx.r3.u32;
  ctx.r11.s64 = -2105933824; // 0x827A0000
  if (Ld8(base, curve) == 0) {
    ctx.r11.s64 = ctx.r11.s64 + 31368;
    St32(base, kStateVa, 0);
    ctx.r10.s64 = 4;
    St32(base, kStateVa + 12, 4);
    return;
  }
  ctx.r10.u64 = Ld32(base, curve + 4);
  ctx.r9.s64 = 0;
  ctx.r8.u64 = Ld32(base, curve + 8);
  ctx.r6.s64 = ctx.r11.s64 + 31368;
  u32 where = 3;
  if (ctx.r10.s32 > 0) {
    ctx.r7.s64 = -2113601536; // 0x82050000
    ctx.fpscr.disableFlushMode();
    const u32 scale_bits = Ld32(base, curve + 12);
    const u32 time_bits = Ld32(base, kStateVa + 8);
    const u64 eps_bits = Ld64(base, kEpsilonVa);
    const u32 zero_bits = Ld32(base, kZeroVa);
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
      key_time = i16(Ld16(base, key));
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
    St64(base, ctx.r1.u32 - 16, u64(i64(key_time)));
  }
  ctx.r11.s64 = where;
  St32(base, ctx.r6.u32 + 0, ctx.r8.u32);
  St32(base, ctx.r6.u32 + 12, ctx.r11.u32);
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

// ---------------------------------------------------------------------------
// sub_8272EC00: the curve's value between two keys (r4 the key before, r5 the
// key after, r3 the curve, f1 the time): a cubic Hermite segment. Each key is
// a 16-bit time, a 16-bit value (an integer or, when the curve's flag is set,
// a half float) and a float tangent. The recompiled body converts every
// integer through the guest stack and the half floats through a vector
// register; this is the same arithmetic on locals, leaving the same
// registers (r5-r11, f0, f1, f4-f13, v0 when a half float was unpacked).

REXCVAR_DEFINE_BOOL(bd_native_anim_interp, true, "Switch",
                    "The animation curve interpolation (sub_8272EC00) as "
                    "native code with the same arithmetic; false runs the "
                    "localized copy.");

namespace {

// vupkd3d128 of one half float, as the recompiled code does it.
inline u32 HalfBits(u32 h) {
  if ((h & 0x7C00) == 0)
    return (h & 0x8000) << 16;
  return ((h & 0x8000) << 16) | (((h & 0x7C00) + 0x1C000) << 13) |
         ((h & 0x03FF) << 13);
}

inline double F32(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return double(f);
}
inline double R(double v) { return double(float(v)); } // frsp / single op

void NativeInterp(PPCContext &ctx, u8 *base) {
  ctx.fpscr.disableFlushMode();
  const u32 r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  const u32 stack = ctx.r1.u32 - 32;

  i64 r7 = i16(Ld16(base, r5 + 0));
  double f0 = F32(Ld32(base, r3 + 12));
  u64 r10 = Ld8(base, r3 + 1);
  u64 r8 = Ld16(base, r5 + 2);
  double f13 = R(double(r7));
  f13 = R(f13 * f0);
  double f5, f7, f12 = ctx.f12.f64, f11 = ctx.f11.f64, f10 = ctx.f10.f64;
  u64 r11, r9, r6 = ctx.r6.u64;
  bool v0_set = false;
  u32 v0_bits = 0;
  if (r10 != 0) {
    v0_bits = HalfBits(u32(r8) & 0xFFFF);
    v0_set = true;
    f5 = F32(v0_bits);
    r11 = stack;
    r9 = stack;
  } else {
    r11 = u64(i64(i16(u16(r8))));
    f12 = double(i64(r11));
    f5 = R(f12);
    r9 = ctx.r9.u64; // reassigned below
  }
  // loc_8272EC6C
  r9 = u64(i64(i16(Ld16(base, r4 + 0))));
  const double f6_in = F32(Ld32(base, r5 + 4));
  double f6 = f6_in;
  r11 = r9;
  f12 = double(i64(r11));
  r11 = Ld16(base, r4 + 2);
  f12 = R(f12);
  f0 = R(f12 * f0);
  u64 r5o = r5;
  if (r10 != 0) {
    v0_bits = HalfBits(u32(r11) & 0xFFFF);
    v0_set = true;
    f7 = F32(v0_bits);
    r6 = stack;
    r5o = stack;
  } else {
    r6 = u64(i64(i16(u16(r11))));
    f12 = double(i64(r6));
    f7 = R(f12);
  }
  // loc_8272ECD4
  double f4 = F32(Ld32(base, r4 + 4));
  if (r10 == 0) {
    r10 = u64(r7) - r9;
    if (!(i32(r10) > 1)) {
      r11 = u64(i64(i16(u16(r11))));
      r10 = u64(i64(i16(u16(r8))));
      r9 = 65536;
      r8 = r10 - r11;
      if (i32(r8) < -32768)
        r10 = r10 + r9;
      // loc_8272ED08
      r7 = 32768;
      r8 = r10 - r11;
      if (i32(r8) > i32(r7))
        r11 = r11 + r9;
      // loc_8272ED20
      r10 = u64(i64(i32(r10)));
      f12 = R(f13 - f0);
      r11 = u64(i64(i32(r11)));
      f11 = double(i64(r11));
      f7 = R(f11);
      f10 = double(i64(r10));
      f5 = R(f10);
      f11 = R(f5 - f7);
      f6 = R(f11 / f12);
      f4 = f6;
    }
  }
  // loc_8272ED58
  f13 = R(f13 - f0);
  f0 = R(ctx.f1.f64 - f0);
  f11 = F32(Ld32(base, 0x82000000u + 4776));
  f13 = R(f11 / f13);
  f12 = R(f0 * f0);
  f10 = R(f13 * f13);
  f10 = R(f12 * f10);
  f12 = R(f12 * f13);
  double f9 = R(f10 * f0);
  const double f8 = R(f9 - f12);
  f9 = R(f9 * f13);
  f13 = F32(Ld32(base, 0x82000000u + 6592));
  f13 = R(f10 * f13);
  f10 = F32(Ld32(base, 0x82000000u + 6588));
  f12 = R(f8 - f12);
  f10 = R(f9 * f10);
  f0 = R(f12 + f0);
  f9 = R(f10 - f13);
  f13 = R(f13 - f10);
  f11 = R(f9 + f11);
  f11 = R(f11 * f7);
  f0 = R(std::fma(f0, f4, f11));
  f0 = R(std::fma(f13, f5, f0));
  const double f1 = R(std::fma(f8, f6, f0));

  ctx.r5.u64 = r5o;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = u64(r7);
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.r11.s64 = -2113929216;
  ctx.f0.f64 = f0;
  ctx.f1.f64 = f1;
  ctx.f4.f64 = f4;
  ctx.f5.f64 = f5;
  ctx.f6.f64 = f6;
  ctx.f7.f64 = f7;
  ctx.f8.f64 = f8;
  ctx.f9.f64 = f9;
  ctx.f10.f64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
  if (v0_set)
    for (int i = 0; i < 4; ++i)
      ctx.v0.u32[i] = v0_bits;
}

bool g_interp_disabled = false;
u32 g_interp_verified = 0;

bool SameInterp(const PPCContext &a, const PPCContext &b) {
  const PPCRegister *ar[] = {&a.r5,  &a.r6,  &a.r7,  &a.r8,  &a.r9,
                             &a.r10, &a.r11, &a.f0,  &a.f1,  &a.f4,
                             &a.f5,  &a.f6,  &a.f7,  &a.f8,  &a.f9,
                             &a.f10, &a.f11, &a.f12, &a.f13};
  const PPCRegister *br[] = {&b.r5,  &b.r6,  &b.r7,  &b.r8,  &b.r9,
                             &b.r10, &b.r11, &b.f0,  &b.f1,  &b.f4,
                             &b.f5,  &b.f6,  &b.f7,  &b.f8,  &b.f9,
                             &b.f10, &b.f11, &b.f12, &b.f13};
  for (size_t i = 0; i < std::size(ar); ++i)
    if (ar[i]->u64 != br[i]->u64)
      return false;
  return std::memcmp(&a.v0, &b.v0, sizeof(a.v0)) == 0;
}

} // namespace

REX_HOOK_RAW(sub_8272EC00) {
  static const bool on = REXCVAR_GET(bd_native_anim_interp);
  if (!on || g_interp_disabled) [[unlikely]] {
    REBLUE_HOT_CALL(sub_8272EC00)(ctx, base);
    return;
  }
  if (g_interp_verified < kVerifyCalls) [[unlikely]] {
    PPCContext ref = ctx;
    REBLUE_HOT_CALL(sub_8272EC00)(ref, base);
    NativeInterp(ctx, base);
    // The half float path leaves the rest of v0 from the stack in the
    // original only before the splat; after it all four lanes are equal.
    if (!SameInterp(ref, ctx)) {
      g_interp_disabled = true;
      std::fprintf(stderr,
                   "[anim-curve] native interpolation differs from the "
                   "game's (call %u: f1 %016llX/%016llX): kept off\n",
                   g_interp_verified,
                   static_cast<unsigned long long>(ctx.f1.u64),
                   static_cast<unsigned long long>(ref.f1.u64));
      ctx = ref;
      return;
    }
    if (++g_interp_verified == kVerifyCalls)
      std::fprintf(stderr, "[anim-curve] native interpolation: %u calls "
                           "identical to the game's: on\n",
                   kVerifyCalls);
    return;
  }
  NativeInterp(ctx, base);
}
