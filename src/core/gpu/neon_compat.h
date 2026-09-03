// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A shim layer over the handful of NEON intrinsics that exist only in AArch64.
// Every name here has two bodies: the plain A64 one, and — under
// DS_A32_SUBSET — one built solely from intrinsics that ARMv7 NEON can also
// express.  Both are compiled for AArch64; the flag does not produce a 32-bit
// build, it restricts the *instruction vocabulary* the kernels are allowed to
// use, so the A32 instruction mix can be measured against the A64 one with the
// JIT, the register file and everything else held fixed.
//
// The A64-only constructs the kernels use, and what replaces them:
//
//   vmaxvq_* / vminvq_* / vmaxv_*   across-vector reduction -> pairwise chain
//   vqtbl1q_u8 / vqtbl2q_u8         Q-table lookup          -> vtbl2 / vtbl4 halves
//   vqtbl4q_u8                      64-byte table           -> two vtbl4 halves, OR-merged
//   v*_high_*                       high-half widening      -> vget_high_* + base op
//   vceqzq_u32                      compare-to-zero         -> vceqq against a zero dup
//   vdivq_f64 & friends             f64 vector divide       -> scalar double divides
//
// The table shims lean on the fact that both vqtblNq_u8 and AArch32's vtblN_u8
// return 0 for an out-of-range index, so a wider table can be assembled by
// OR-ing the results of two narrower lookups with the index biased between
// them.  That is the same rule the reference kernels rely on, so the shims are
// value-identical, not merely close.
#pragma once

#include "core/types.h"

#include <arm_neon.h>

#ifndef DS_A32_SUBSET
#define DS_A32_SUBSET 0
#endif

namespace ds::gpu::kern::compat {

// ---- across-vector reductions --------------------------------------------
// AArch32 has no ADDV/MAXV/MINV; the idiom is log2(lanes) pairwise steps on
// D registers, which is what the shims below spell out.

[[gnu::always_inline]] inline u8 maxv_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmax_u8(vget_low_u8(v), vget_high_u8(v));
  d = vpmax_u8(d, d);
  d = vpmax_u8(d, d);
  return vget_lane_u8(vpmax_u8(d, d), 0);
#else
  return vmaxvq_u8(v);
#endif
}

[[gnu::always_inline]] inline u8 minv_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmin_u8(vget_low_u8(v), vget_high_u8(v));
  d = vpmin_u8(d, d);
  d = vpmin_u8(d, d);
  return vget_lane_u8(vpmin_u8(d, d), 0);
#else
  return vminvq_u8(v);
#endif
}

[[gnu::always_inline]] inline u8 maxv_u8(uint8x8_t v) {
#if DS_A32_SUBSET
  uint8x8_t d = vpmax_u8(v, v);
  d = vpmax_u8(d, d);
  return vget_lane_u8(vpmax_u8(d, d), 0);
#else
  return vmaxv_u8(v);
#endif
}

[[gnu::always_inline]] inline u16 maxv_u16(uint16x8_t v) {
#if DS_A32_SUBSET
  uint16x4_t d = vpmax_u16(vget_low_u16(v), vget_high_u16(v));
  d = vpmax_u16(d, d);
  return vget_lane_u16(vpmax_u16(d, d), 0);
#else
  return vmaxvq_u16(v);
#endif
}

[[gnu::always_inline]] inline u32 maxv_u32(uint32x4_t v) {
#if DS_A32_SUBSET
  uint32x2_t d = vpmax_u32(vget_low_u32(v), vget_high_u32(v));
  return vget_lane_u32(vpmax_u32(d, d), 0);
#else
  return vmaxvq_u32(v);
#endif
}

[[gnu::always_inline]] inline u32 addv_u32(uint32x4_t v) {
#if DS_A32_SUBSET
  const uint32x2_t d = vadd_u32(vget_low_u32(v), vget_high_u32(v));
  return vget_lane_u32(vpadd_u32(d, d), 0);
#else
  return vaddvq_u32(v);
#endif
}

// A 16-lane byte plane is uniform.  Spelt out as its own shim because the
// kernels ask this question far more often than they ask for either extreme,
// and the A32 form can answer it with one pairwise chain over a difference
// instead of the two chains a max == min pair would cost.
[[gnu::always_inline]] inline bool uniform_u8(uint8x16_t v) {
#if DS_A32_SUBSET
  return maxv_u8(veorq_u8(v, vdupq_n_u8(vgetq_lane_u8(v, 0)))) == 0;
#else
  return vmaxvq_u8(v) == vminvq_u8(v);
#endif
}

// ---- table lookups --------------------------------------------------------
// vtbl2_u8 covers a 16-byte table, vtbl4_u8 a 32-byte one, both 8 indices at
// a time and both zeroing out-of-range lanes.

[[gnu::always_inline]] inline uint8x16_t tbl1q_u8(uint8x16_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  const uint8x8x2_t tt = {{vget_low_u8(t), vget_high_u8(t)}};
  return vcombine_u8(vtbl2_u8(tt, vget_low_u8(idx)), vtbl2_u8(tt, vget_high_u8(idx)));
#else
  return vqtbl1q_u8(t, idx);
#endif
}

[[gnu::always_inline]] inline uint8x16_t tbl2q_u8(uint8x16x2_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  const uint8x8x4_t tt = {{vget_low_u8(t.val[0]), vget_high_u8(t.val[0]),
                           vget_low_u8(t.val[1]), vget_high_u8(t.val[1])}};
  return vcombine_u8(vtbl4_u8(tt, vget_low_u8(idx)), vtbl4_u8(tt, vget_high_u8(idx)));
#else
  return vqtbl2q_u8(t, idx);
#endif
}

[[gnu::always_inline]] inline uint8x16_t tbl4q_u8(uint8x16x4_t t, uint8x16_t idx) {
#if DS_A32_SUBSET
  // 64 bytes is twice what vtbl4 can address: look the index up in both
  // 32-byte halves, biasing the second by 32.  An index below 32 misses the
  // high half (it wraps to >= 224) and one at or above 32 misses the low
  // half, so exactly one lookup contributes and the two can be OR-ed.
  const uint8x8x4_t lo = {{vget_low_u8(t.val[0]), vget_high_u8(t.val[0]),
                           vget_low_u8(t.val[1]), vget_high_u8(t.val[1])}};
  const uint8x8x4_t hi = {{vget_low_u8(t.val[2]), vget_high_u8(t.val[2]),
                           vget_low_u8(t.val[3]), vget_high_u8(t.val[3])}};
  const uint8x16_t bias = vsubq_u8(idx, vdupq_n_u8(32));
  const uint8x16_t a = vcombine_u8(vtbl4_u8(lo, vget_low_u8(idx)), vtbl4_u8(lo, vget_high_u8(idx)));
  const uint8x16_t b = vcombine_u8(vtbl4_u8(hi, vget_low_u8(bias)), vtbl4_u8(hi, vget_high_u8(bias)));
  return vorrq_u8(a, b);
#else
  return vqtbl4q_u8(t, idx);
#endif
}

// ---- high-half widening ---------------------------------------------------
// Pure sugar: on AArch32 the high half is extracted first and the base op
// applied to the D register.  Identical results either way.

[[gnu::always_inline]] inline uint64x2_t mull_high_u32(uint32x4_t a, uint32x4_t b) {
#if DS_A32_SUBSET
  return vmull_u32(vget_high_u32(a), vget_high_u32(b));
#else
  return vmull_high_u32(a, b);
#endif
}

[[gnu::always_inline]] inline int64x2_t mull_high_s32(int32x4_t a, int32x4_t b) {
#if DS_A32_SUBSET
  return vmull_s32(vget_high_s32(a), vget_high_s32(b));
#else
  return vmull_high_s32(a, b);
#endif
}

[[gnu::always_inline]] inline int64x2_t mlal_high_s32(int64x2_t acc, int32x4_t a, int32x4_t b) {
#if DS_A32_SUBSET
  return vmlal_s32(acc, vget_high_s32(a), vget_high_s32(b));
#else
  return vmlal_high_s32(acc, a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t mull_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vmull_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vmull_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t mlal_high_u8(uint16x8_t acc, uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vmlal_u8(acc, vget_high_u8(a), vget_high_u8(b));
#else
  return vmlal_high_u8(acc, a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t addl_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vaddl_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vaddl_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t subl_high_u8(uint8x16_t a, uint8x16_t b) {
#if DS_A32_SUBSET
  return vsubl_u8(vget_high_u8(a), vget_high_u8(b));
#else
  return vsubl_high_u8(a, b);
#endif
}

[[gnu::always_inline]] inline uint16x8_t movl_high_u8(uint8x16_t a) {
#if DS_A32_SUBSET
  return vmovl_u8(vget_high_u8(a));
#else
  return vmovl_high_u8(a);
#endif
}

[[gnu::always_inline]] inline uint32x4_t movl_high_u16(uint16x8_t a) {
#if DS_A32_SUBSET
  return vmovl_u16(vget_high_u16(a));
#else
  return vmovl_high_u16(a);
#endif
}

[[gnu::always_inline]] inline int32x4_t movl_high_s16(int16x8_t a) {
#if DS_A32_SUBSET
  return vmovl_s16(vget_high_s16(a));
#else
  return vmovl_high_s16(a);
#endif
}

// The shift count has to survive as a literal into the intrinsic, so this one
// is a template rather than a plain parameter.
template <int N>
[[gnu::always_inline]] inline uint32x4_t shll_high_n_u16(uint16x8_t a) {
#if DS_A32_SUBSET
  return vshll_n_u16(vget_high_u16(a), N);
#else
  return vshll_high_n_u16(a, N);
#endif
}

// ---- lane broadcast ------------------------------------------------------
// AArch32's VDUP.32 can only name a lane of a D register, so a lane in the
// upper half is reached by extracting that half first.  N is a template
// parameter for the same reason the shift count above is.
template <int N>
[[gnu::always_inline]] inline int32x4_t dup_laneq_s32(int32x4_t v) {
#if DS_A32_SUBSET
  return vdupq_lane_s32(N < 2 ? vget_low_s32(v) : vget_high_s32(v), N & 1);
#else
  return vdupq_laneq_s32(v, N);
#endif
}

// ---- compare against zero -------------------------------------------------
[[gnu::always_inline]] inline uint32x4_t ceqz_u32(uint32x4_t a) {
#if DS_A32_SUBSET
  return vceqq_u32(a, vdupq_n_u32(0));
#else
  return vceqzq_u32(a);
#endif
}

// ---- exact unsigned division ----------------------------------------------
// The one place where the A32 subset cannot match the A64 shape at all:
// AArch32 NEON has no double-precision vector type and no vector divide of
// any width, so the f64 quotient the kernels use to reproduce the reference's
// integer division exactly has to be done a lane at a time on the scalar VFP
// unit.  Same values (the f64 quotient of two u32 values, truncated, is the
// exact integer quotient), very different cost — this is the shim to watch in
// a span-heavy profile.
//
// The kernels still compile for AArch64, so the vectoriser will happily fuse a
// plain loop of four scalar divides straight back into the two f64 vector
// divides the subset is meant to exclude.  keep_scalar pins each quotient in a
// scalar register with an empty asm, which is what makes the measurement mean
// anything.
[[gnu::always_inline]] inline double keep_scalar(double v) {
  asm("" : "+w"(v));
  return v;
}

[[gnu::always_inline]] inline uint32x4_t udiv_exact(uint32x4_t num, uint32x4_t den) {
#if DS_A32_SUBSET
  u32 n[4], d[4], q[4];
  vst1q_u32(n, num);
  vst1q_u32(d, den);
  for (int i = 0; i < 4; ++i)
    q[i] = static_cast<u32>(keep_scalar(static_cast<double>(n[i]) / static_cast<double>(d[i])));
  return vld1q_u32(q);
#else
  const float64x2_t nlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(num))), nhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(num)));
  const float64x2_t dlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(den))), dhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(den)));
  const uint64x2_t qlo = vcvtq_u64_f64(vdivq_f64(nlo, dlo)), qhi = vcvtq_u64_f64(vdivq_f64(nhi, dhi));
  return vcombine_u32(vmovn_u64(qlo), vmovn_u64(qhi));
#endif
}

// (u64 lanes) / d, exact, for products below 2^53.
[[gnu::always_inline]] inline uint64x2_t udiv64_exact(uint64x2_t n, double d0, double d1) {
#if DS_A32_SUBSET
  u64 n2[2], q[2];
  vst1q_u64(n2, n);
  q[0] = static_cast<u64>(keep_scalar(static_cast<double>(n2[0]) / d0));
  q[1] = static_cast<u64>(keep_scalar(static_cast<double>(n2[1]) / d1));
  return vld1q_u64(q);
#else
  const float64x2_t d = vsetq_lane_f64(d1, vdupq_n_f64(d0), 1);
  return vcvtq_u64_f64(vdivq_f64(vcvtq_f64_u64(n), d));
#endif
}

} // namespace ds::gpu::kern::compat
