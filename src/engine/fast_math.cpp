/**
 * @file    engine/fast_math.cpp
 * @brief   Native versions of the game's math helpers that the profile shows
 *          on the Main Thread, with the same arithmetic as the recompiled
 *          code (same operations, same order, same roundings).
 *
 * bdSinCos (0x82168610) was ~8% of the Main Thread in the slow field scenes:
 * the recompiled body keeps its VMX polynomial in emulated vector registers,
 * goes through the guest stack nine times and switches the denormal flush
 * mode twice per call (two writes of a system register).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <climits>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"

REXCVAR_DEFINE_BOOL(bd_native_sincos, true, "Switch",
                    "bdSinCos (the game's sine/cosine) as native code with "
                    "the same arithmetic; false runs the recompiled function.");

namespace {

// Guest constants bdSinCos reads (big-endian, in the XEX's read-only data).
constexpr u32 kPiBits = 0x40490FDB;      // rlwimi of 0x01243F6D: pi, x's sign
constexpr u32 kInvTwoPiVa = 0x8208EA50;  // lis -32247, -5552
constexpr u32 kTwoPiVa = 0x8205E324;     // lis -32250, -7388
constexpr u32 kLeadVa = 0x820551AC;      // lis -32251, 20908: element 0
// The six coefficient vectors (lvx128 v11, v10, v9, v8, v7, v6).
constexpr u32 kCoefVa[6] = {0x820658B0, 0x82065880, 0x820658C0,
                            0x82065890, 0x820658D0, 0x820658A0};

struct Constants {
  float invTwoPi, twoPi, lead;
  // Big-endian element order: e[0] is the first float in memory.
  float k11[4], k10[4], k9[4], k8[4], k7[4], k6[4];
#if defined(__aarch64__)
  // The same in lane order (lane i = element 3 - i).
  float32x4_t q11, q10, q9, q8, q7, q6;
#endif
};

bool LoadConstants(Constants &c) {
  auto f = [](u32 va, float &out) {
    const auto *p = bd::mem::at<const be_u32>(va);
    if (!p)
      return false;
    const u32 bits = *p;
    std::memcpy(&out, &bits, 4);
    return true;
  };
  float *vec[6] = {c.k11, c.k10, c.k9, c.k8, c.k7, c.k6};
  bool ok = f(kInvTwoPiVa, c.invTwoPi) && f(kTwoPiVa, c.twoPi) &&
            f(kLeadVa, c.lead);
  for (int v = 0; v < 6 && ok; ++v)
    for (int e = 0; e < 4 && ok; ++e)
      ok = f(kCoefVa[v] + 4 * e, vec[v][e]);
#if defined(__aarch64__)
  float32x4_t *q[6] = {&c.q11, &c.q10, &c.q9, &c.q8, &c.q7, &c.q6};
  for (int v = 0; v < 6; ++v)
    *q[v] = float32x4_t{vec[v][3], vec[v][2], vec[v][1], vec[v][0]};
#endif
  return ok;
}

const Constants *GetConstants() {
  static Constants c;
  static const bool ok = LoadConstants(c);
  return ok ? &c : nullptr;
}

// The vector part runs with denormals flushed to zero (the original turns
// the flush mode on around it): inputs and results of each operation.
inline float FZ(float v) {
  return std::fabs(v) < FLT_MIN ? std::copysign(0.0f, v) : v;
}
inline float MulZ(float a, float b) { return FZ(FZ(a) * FZ(b)); }
#if defined(__aarch64__)
inline float32x4_t FZ4(float32x4_t v) {
  // |v| < FLT_MIN is false for a NaN, which stays as it is.
  const uint32x4_t tiny = vcaltq_f32(v, vdupq_n_f32(FLT_MIN));
  const float32x4_t signed_zero = vreinterpretq_f32_u32(
      vandq_u32(vreinterpretq_u32_f32(v), vdupq_n_u32(0x80000000u)));
  return vbslq_f32(tiny, signed_zero, v);
}
#endif
inline float AddZ(float a, float b) { return FZ(FZ(a) + FZ(b)); }

// vmsum4fp128 as the recompiled code runs it (simde_mm_dp_ps 0xFF: a lane
// product, then vaddvq_f32 over the lanes). The lanes hold the elements in
// reverse (lane i = element 3 - i), and the sum is (l0 + l1) + (l2 + l3).
inline float Dot(const float a[4], const float b[4]) {
  const float p0 = MulZ(a[3], b[3]), p1 = MulZ(a[2], b[2]);
  const float p2 = MulZ(a[1], b[1]), p3 = MulZ(a[0], b[0]);
  return AddZ(AddZ(p0, p1), AddZ(p2, p3));
}

inline void Mul(float out[4], const float a[4], float s) {
  for (int i = 0; i < 4; ++i)
    out[i] = MulZ(a[i], s);
}

inline float AddF(float a, float b) {
  return float(double(a) + double(b));
}

} // namespace

REX_EXTERN(__imp__bdSinCos);

namespace {

void NativeSinCos(const Constants *k, double x, float &sin_v, float &cos_v) {
  // stfs f1; rlwimi: pi carrying the sign of (float)x.
  u32 sign_bits;
  const float xf = float(x);
  std::memcpy(&sign_bits, &xf, 4);
  const u32 h_bits = (sign_bits & 0x80000000u) | kPiBits;
  float h;
  std::memcpy(&h, &h_bits, 4);

  // fadds, fmuls, fctiwz, fcfid + frsp, fnmsubs.
  const float f12 = float(double(h) + x);
  const double scaled = double(float(double(f12) * double(k->invTwoPi)));
  i32 n;
  if (std::isnan(scaled))
    n = INT_MIN;
  else if (scaled >= double(INT_MAX))
    n = INT_MAX;
  else if (scaled <= double(INT_MIN))
    n = INT_MIN;
  else
    n = i32(scaled); // truncation toward zero
  const float nf = float(double(n));
  const float r = float(-std::fma(double(nf), double(k->twoPi), -x));
  const float r2 = float(double(r) * double(r));
  const float r3 = float(double(r2) * double(r));

  // The vector part: v13 = {lead, r, r^2, r^3}.
#if defined(__aarch64__)
  // NEON, lanes as the recompiled code holds them (lane i = element 3 - i),
  // products flushed like the original's flush mode does (vector compare and
  // select: no FPCR write), dot products summed by vaddvq_f32 as simde does.
  const float32x4_t v13 = FZ4(float32x4_t{r3, r2, r, k->lead});
  const float32x4_t v0 = FZ4(vmulq_f32(v13, v13));
  const float32x4_t v12 = FZ4(vmulq_f32(v0, vdupq_laneq_f32(v13, 2)));
  const float d13 = vaddvq_f32(FZ4(vmulq_f32(v0, k->q11)));    // -64
  const float d11 = vaddvq_f32(FZ4(vmulq_f32(v12, k->q10)));   // -16
  const float32x4_t sv =
      FZ4(vmulq_f32(vdupq_laneq_f32(v0, 0), vdupq_laneq_f32(v13, 1)));
  const float32x4_t v0b = FZ4(vmulq_f32(v0, sv));
  const float32x4_t v12b = FZ4(vmulq_f32(v12, sv));
  const float32x4_t v11b = FZ4(vmulq_f32(v0b, sv));
  const float32x4_t v13b = FZ4(vmulq_f32(v12b, sv));
  const float d0 = vaddvq_f32(FZ4(vmulq_f32(v0b, k->q9)));     // -80
  const float d12 = vaddvq_f32(FZ4(vmulq_f32(v12b, k->q8)));   // -48
  const float d13b = vaddvq_f32(FZ4(vmulq_f32(v13b, k->q6)));  // -32
  const float d0b = vaddvq_f32(FZ4(vmulq_f32(v11b, k->q7)));   // -96
#else
  const float v13[4] = {k->lead, r, r2, r3};
  float v0[4], v12[4];
  for (int i = 0; i < 4; ++i)
    v0[i] = MulZ(v13[i], v13[i]);
  Mul(v12, v0, v13[1]);                  // vspltw v13,1: r
  const float d13 = Dot(v0, k->k11);     // stored at -64
  const float d11 = Dot(v12, k->k10);    // stored at -16
  const float s = MulZ(v0[3], v13[2]);   // vspltw v0,3 * vspltw v13,2
  float v0b[4], v12b[4], v11b[4], v13b[4];
  Mul(v0b, v0, s);
  Mul(v12b, v12, s);
  Mul(v11b, v0b, s);
  const float d0 = Dot(v0b, k->k9);      // -80
  Mul(v13b, v12b, s);
  const float d12 = Dot(v12b, k->k8);    // -48
  const float d13b = Dot(v13b, k->k6);   // -32
  const float d0b = Dot(v11b, k->k7);    // -96

#endif

  cos_v = AddF(AddF(d0b, d0), d13);
  sin_v = AddF(AddF(d13b, d12), d11);
}

// Once, on the first call: the native against the recompiled function over
// a sweep of angles, bit for bit. Any difference and the native stays off.
bool SelfTest(PPCContext &ctx, u8 *base, const Constants *k) {
  PPCContext t = ctx;
  t.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull; // below the caller's frame
  const u32 out_sin = ctx.r1.u32 - 0x200, out_cos = ctx.r1.u32 - 0x1F0;
  u32 tested = 0, differ = 0;
  for (i32 i = -40000; i <= 40000; ++i) {
    const float angle = float(i) * 0.000785398163f * 3.0f; // about +-94 rad
    PPCContext c = t;
    c.f1.f64 = double(angle);
    c.r3.u64 = out_sin;
    c.r4.u64 = out_cos;
    __imp__bdSinCos(c, base);
    const u32 ref_sin = bd::mem::load<u32>(out_sin);
    const u32 ref_cos = bd::mem::load<u32>(out_cos);
    float s, co;
    NativeSinCos(k, double(angle), s, co);
    u32 ns, nc;
    std::memcpy(&ns, &s, 4);
    std::memcpy(&nc, &co, 4);
    ++tested;
    if (ns != ref_sin || nc != ref_cos) {
      if (differ < 4)
        std::fprintf(stderr,
                     "[fast-math] bdSinCos(%.9g): native %08X %08X, game "
                     "%08X %08X\n",
                     double(angle), ns, nc, ref_sin, ref_cos);
      ++differ;
    }
  }
  ctx.fpscr.disableFlushMode();
  std::fprintf(stderr, "[fast-math] bdSinCos native: %u of %u angles differ "
               "from the game's: %s\n",
               differ, tested, differ ? "kept off" : "on");
  return differ == 0;
}

} // namespace

// bdSinCos(float *sin_out = r3, float *cos_out = r4, float angle = f1).
REX_HOOK_RAW(bdSinCos) {
  static const bool on = REXCVAR_GET(bd_native_sincos);
  static const Constants *k = on ? GetConstants() : nullptr;
  static const bool ok = k && SelfTest(ctx, base, k);
  if (!ok) [[unlikely]] {
    __imp__bdSinCos(ctx, base);
    return;
  }
  // The flush mode the original leaves on return (it turns flushing off on
  // entry and again at the end).
  ctx.fpscr.disableFlushMode();
  float sin_v, cos_v;
  NativeSinCos(k, ctx.f1.f64, sin_v, cos_v);
  u32 bits;
  std::memcpy(&bits, &cos_v, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(base + ctx.r4.u32, &bits, 4);
  std::memcpy(&bits, &sin_v, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(base + ctx.r3.u32, &bits, 4);
}
