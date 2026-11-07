// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Both profiles are "gamma in, 3x3 matrix, curve out":
//   linear = clamp01(c ^ gamma_in * lum)
//   mixed  = M * linear
//   out    = clamp01(lo + (hi - lo) * clamp01(mixed) ^ (1 / gamma_out))
// which is separable per input channel, so kern::colour_correct needs only
// a 64-entry input curve, the matrix, and the output curve.
//
// nds: hunterk's nds-color shader as modified by Pokefan531 (public domain),
// the colour part of ROCKNIX's lcd1x-nds-color (Cainam-git, from
// jdgleaver/drastic_ds_shaders): gamma 1.91 in and out, lum 0.89. The GLSL
// mat3 there is column-major; the rows below are its transpose.
//
// nds_lite: the ds_lite tone of josegonzalez's minui-nintendo-dsperate-pak
// (PR 15, "colour and pattern shaders"), itself after kleinrein/rgds-core
// (GPL-3.0-or-later): gamma 2.2 in, a mild channel bleed, gamma 2.3 out,
// black and white lifted to the panel's.
#include "core/gpu/colour.h"

#include <algorithm>
#include <cassert>
#include <cmath>

extern "C" double ds_pow_compat(double x, double y);

namespace ds::gpu {

namespace {

struct Model {
  double gin, lum;
  double m[9];
  double gout;
  double lo[3], hi[3];
};

constexpr Model kNds = {1.91, 0.89,
                        {0.87, 0.255, -0.125,
                         0.10, 0.645, 0.255,
                         0.10, 0.17, 0.73},
                        1.91, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
constexpr Model kNdsLite = {2.2, 1.0,
                            {0.90, 0.08, 0.02,
                             0.05, 0.89, 0.06,
                             0.03, 0.07, 0.90},
                            2.3, {0.025, 0.03, 0.04}, {0.97, 0.98, 1.0}};

const Model& model(ColourProfile p) { return p == ColourProfile::NdsLite ? kNdsLite : kNds; }

double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
double powp(double x, double e) { return x <= 0.0 ? 0.0 : ds_pow_compat(x, e); }
// What the output stage shows for a 6-bit channel: (c << 2) | (c >> 4).
double expand6(u32 c6) { return static_cast<double>((c6 << 2) | (c6 >> 4)) / 255.0; }
double curve_in(const Model& md, double c) { return clamp01(powp(c, md.gin) * md.lum); }
double curve_out(const Model& md, int k, double v) {
  return clamp01(md.lo[k] + (md.hi[k] - md.lo[k]) * powp(clamp01(v), 1.0 / md.gout));
}
u8 to_byte(double v) { return static_cast<u8>(std::lround(clamp01(v) * 255.0)); }

constexpr const char* kNames[] = {"off", "nds", "nds_lite"};
static_assert(sizeof kNames / sizeof kNames[0] == static_cast<size_t>(ColourProfile::Count));

} // namespace

bool parse_colour_profile(const std::string& v, ColourProfile& out) {
  for (u8 i = 0; i < static_cast<u8>(ColourProfile::Count); ++i)
    if (v == kNames[i]) { out = static_cast<ColourProfile>(i); return true; }
  out = ColourProfile::Off;
  return false;
}

const char* colour_profile_name(ColourProfile p) { return p < ColourProfile::Count ? kNames[static_cast<u8>(p)] : "off"; }

void build_colour_lut(ColourProfile p, kern::ColourLut& out) {
  const Model& md = model(p);
  for (u32 c = 0; c < 64; ++c) {
    const u32 l = std::min<u32>(static_cast<u32>(std::lround(curve_in(md, expand6(c)) * kern::ColourLut::ONE)), 32767);
    out.lin_lo[c] = static_cast<u8>(l & 0xFF);
    out.lin_hi[c] = static_cast<u8>(l >> 8);
  }
  for (u32 k = 0; k < 9; ++k) out.m[k] = static_cast<s16>(std::lround(md.m[k] * 32768.0));
  // The kernel's 16-bit sum: the outer two terms cannot overflow, the middle one saturates.
  for (u32 k = 0; k < 3; ++k) assert(std::fabs(md.m[3 * k]) + std::fabs(md.m[3 * k + 2]) <= 1.0 && std::fabs(md.m[3 * k + 1]) < 1.0);
  constexpr u32 F = kern::ColourLut::FINE;
  constexpr double one = kern::ColourLut::ONE;
  for (int k = 0; k < 3; ++k) {
    for (u32 s = 0; s < F; ++s) out.curve[k][s] = to_byte(curve_out(md, k, s / one));
    for (u32 q = F / 8; q < kern::ColourLut::ONE / 8; ++q) out.curve[k][kern::colour_index(q << 3)] = to_byte(curve_out(md, k, (q * 8 + 3.5) / one));
  }
}

void colour_model(ColourProfile p, const u32 in6[3], u8 out[3]) {
  const Model& md = model(p);
  double l[3];
  for (int c = 0; c < 3; ++c) l[c] = curve_in(md, expand6(in6[c] & 63));
  for (int k = 0; k < 3; ++k) out[k] = to_byte(curve_out(md, k, md.m[3 * k] * l[0] + md.m[3 * k + 1] * l[1] + md.m[3 * k + 2] * l[2]));
}

} // namespace ds::gpu
