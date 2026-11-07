/**
 * @file    platform/switch/hot_flush_skip.h
 * @brief   Checked vector float operations for the localized copies
 *          (bd_cpu_flush_skip, tools/switch/localize.py speculate_flush).
 *
 * Included by hot_natives.cpp and by tools/switch/localize_check.py; needs
 * simde's SSE4.1 layer (rex/ppc/context.h brings it).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Flush-mode skip (bd_cpu_flush_skip, localize.py speculate_flush).
//
// The recompiled code runs vector (VMX) float math with FPCR.FZ set and
// scalar FPU math with it clear, and writes FPCR at every switch: on the
// Cortex-A57 an msr fpcr costs a pipeline drain, and short vector sequences
// between scalar code (a vector subtract, a dot product) paid two of them.
// FZ only changes a result when a denormal is read or produced: it reads
// denormal inputs as zero, and returns zero for a result whose magnitude
// before rounding is below 2^-126. So the sequence runs with FZ clear and
// every input, intermediate and result of its float operations is checked:
// none in 0 < |x| <= 2^-126 means each operation rounded the same as with FZ
// (a result that rounds to exactly 2^-126 counts as suspect, since FZ tests
// before rounding). Otherwise that statement turns FZ on (as the original
// did) and computes again, its inputs and result passed through empty asm so
// the compiler can neither reuse the first result nor move the operation
// past the FPCR write; the sequence's closing switch back to scalar then
// writes FPCR as before.
namespace reblue_nz {
#if defined(__aarch64__)
using Acc = uint32x4_t;
static inline Acc Start() { return vdupq_n_u32(0xFFFFFFFFu); }
// (bits << 1) - 1 drops the sign: zero wraps to the top, 0 < |x| <= 2^-126
// lands below 0x01000000.
static inline void See(Acc &a, simde__m128 x) {
  const uint32x4_t u = vreinterpretq_u32_f32(simde__m128_to_neon_f32(x));
  a = vminq_u32(a, vsubq_u32(vshlq_n_u32(u, 1), vdupq_n_u32(1)));
}
static inline bool Ok(Acc a) { return vminvq_u32(a) >= 0x01000000u; }
static inline simde__m128 Opaque(simde__m128 v) {
  float32x4_t f = simde__m128_to_neon_f32(v);
  __asm__ __volatile__("" : "+w"(f));
  return simde__m128_from_neon_f32(f);
}
#else
using Acc = int;
static inline Acc Start() { return 0; }
static inline void See(Acc &, simde__m128) {}
static inline bool Ok(Acc) { return false; }
static inline simde__m128 Opaque(simde__m128 v) {
  __asm__ __volatile__("" : "+m"(v));
  return v;
}
#endif
#define REBLUE_NZ_BINARY(op)                                                   \
  static inline simde__m128 op(Acc &a, simde__m128 x, simde__m128 y) {         \
    See(a, x);                                                                 \
    See(a, y);                                                                 \
    const simde__m128 r = simde_mm_##op##_ps(x, y);                            \
    See(a, r);                                                                 \
    return r;                                                                  \
  }                                                                            \
  static inline simde__m128 fz_##op(simde__m128 x, simde__m128 y) {            \
    return Opaque(simde_mm_##op##_ps(Opaque(x), Opaque(y)));                   \
  }
REBLUE_NZ_BINARY(mul)
REBLUE_NZ_BINARY(add)
REBLUE_NZ_BINARY(sub)
REBLUE_NZ_BINARY(div)
REBLUE_NZ_BINARY(max)
REBLUE_NZ_BINARY(min)
// Compares return masks (all ones reads as a NaN, zero as zero: never
// suspect); only their inputs matter.
REBLUE_NZ_BINARY(cmpeq)
REBLUE_NZ_BINARY(cmpgt)
REBLUE_NZ_BINARY(cmpge)
REBLUE_NZ_BINARY(cmplt)
REBLUE_NZ_BINARY(cmple)
#undef REBLUE_NZ_BINARY
static inline simde__m128 sqrt(Acc &a, simde__m128 x) {
  See(a, x);
  const simde__m128 r = simde_mm_sqrt_ps(x);
  See(a, r);
  return r;
}
static inline simde__m128 fz_sqrt(simde__m128 x) {
  return Opaque(simde_mm_sqrt_ps(Opaque(x)));
}
// simde's dp_ps on AArch64: the products, masked by imm's high nibble, summed
// by vaddvq_f32 as (p0 + p1) + (p2 + p3), the sum masked by the low nibble.
template <int imm>
static inline simde__m128 dp(Acc &a, simde__m128 x, simde__m128 y) {
  See(a, x);
  See(a, y);
#if defined(__aarch64__)
  const uint32x4_t keep = {(imm & 0x10) ? ~0u : 0u, (imm & 0x20) ? ~0u : 0u,
                           (imm & 0x40) ? ~0u : 0u, (imm & 0x80) ? ~0u : 0u};
  const float32x4_t p = vmulq_f32(simde__m128_to_neon_f32(x),
                                  simde__m128_to_neon_f32(y));
  const float32x4_t m =
      vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(p), keep));
  See(a, simde__m128_from_neon_f32(m));
  See(a, simde__m128_from_neon_f32(vpaddq_f32(m, m)));
#endif
  const simde__m128 r = simde_mm_dp_ps(x, y, imm);
  See(a, r);
  return r;
}
template <int imm> static inline simde__m128 fz_dp(simde__m128 x, simde__m128 y) {
  return Opaque(simde_mm_dp_ps(Opaque(x), Opaque(y), imm));
}
} // namespace reblue_nz
