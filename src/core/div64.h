// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 64-bit divides without libgcc on 32-bit ARM. ARMv7 has a 32/32 hardware
// divide and nothing wider, so every s64 / s32 in C++ becomes a call to
// __aeabi_ldivmod -> __udivmoddi4, a shift-subtract loop of a hundred-odd
// instructions on the A7. These do the division in VFP double instead and
// correct the result with an integer remainder check, so they return exactly
// what the C++ operator does (truncation toward zero). Out-of-range operands
// take the plain operator. Elsewhere they are the plain operator: AArch64 and
// x86 divide 64-bit in one instruction, and their code must not move.
#pragma once
#include "core/types.h"

namespace ds {

#if defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)

// n / d. Exact through double while |n| < 2^52 (the numerator converts without
// rounding) and the quotient fits 31 bits (the double -> s32 conversion is one
// instruction): the rounded quotient is then within 2^-21 of the true one, so
// truncating it is off by at most one, which the remainder fixes.
[[gnu::always_inline]] inline s64 div_s64(s64 n, s32 d) {
  const s32 hi = static_cast<s32>(n >> 32);
  if (d != 0 && hi >= -(1 << 19) && hi < (1 << 19)) {
    const double qd = (static_cast<double>(hi) * 4294967296.0 + static_cast<double>(static_cast<u32>(n))) / d;
    if (qd > -2147483000.0 && qd < 2147483000.0) {
      s64 q = static_cast<s32>(qd);
      const s64 r = n - q * d;
      const s64 ad = d < 0 ? -static_cast<s64>(d) : d;
      const s64 away = d < 0 ? -1 : 1;   // the step that moves q*d up by |d|
      if (n >= 0) { if (r < 0) q -= away; else if (r >= ad) q += away; }
      else { if (r > 0) q += away; else if (-r >= ad) q -= away; }
      return q;
    }
  }
  return n / d;
}

// n / d, unsigned; the same argument with a 32-bit unsigned quotient.
[[gnu::always_inline]] inline u64 div_u64(u64 n, u32 d) {
  const u32 hi = static_cast<u32>(n >> 32);
  if (d != 0 && hi < (1u << 20)) {
    const double qd = (static_cast<double>(hi) * 4294967296.0 + static_cast<double>(static_cast<u32>(n))) / static_cast<double>(d);
    if (qd < 4294967000.0) {
      u64 q = static_cast<u32>(qd);
      const u64 qd64 = q * d;
      if (qd64 > n) --q;
      else if (n - qd64 >= d) ++q;
      return q;
    }
  }
  return n / d;
}

// ceil(2^32 / x) for x >= 2: 2^32 = q*x + r gives (2^32 - 1) / x = q - (r == 0),
// so adding one is the ceiling either way -- a 32-bit divide.
[[gnu::always_inline]] inline u32 recip_ceil32(u32 x) { return 0xFFFFFFFFu / x + 1; }

#else

[[gnu::always_inline]] inline s64 div_s64(s64 n, s32 d) { return n / d; }
[[gnu::always_inline]] inline u64 div_u64(u64 n, u32 d) { return n / d; }
[[gnu::always_inline]] inline u32 recip_ceil32(u32 x) { return static_cast<u32>(((1ull << 32) + x - 1) / x); }

#endif

} // namespace ds
