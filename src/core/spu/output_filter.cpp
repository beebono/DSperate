// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/spu/output_filter.h"

#include <algorithm>
#include <cmath>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace ds::spu {

namespace {

constexpr double PI = 3.14159265358979323846;

// Each model's band, in cycles per input frame, and the Kaiser beta that puts
// the transition between those edges with TAPS taps: transition width
// (A - 7.95) / (2.285 * 2pi * (TAPS - 1)) for stopband attenuation A, beta
// 0.1102 * (A - 8.7) (Kaiser's formulas).
struct Band { double pass, stop; };
constexpr Band band_of(OutputFilter::Model m) {
  return m == OutputFilter::Model::Dsi ? Band{0.45, 0.55}   // TSC2117 filter A
                                       : Band{0.42, 0.50};  // the DS's Nyquist
}

double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < sum * 1e-12) break; }
  return sum;
}

// The prototype kernel at distance d input frames from the output time.
double kernel(double d, double fc, double beta) {
  constexpr double half = OutputFilter::TAPS / 2.0;
  if (std::fabs(d) >= half) return 0.0;
  const double x = 2.0 * fc * d;
  const double sinc = x == 0.0 ? 1.0 : std::sin(PI * x) / (PI * x);
  const double u = d / half;
  return 2.0 * fc * sinc * bessel_i0(beta * std::sqrt(1.0 - u * u)) / bessel_i0(beta);
}

struct Design { double fc, beta; };
Design design_of(OutputFilter::Model m, double scale) {
  // Narrowed for a device slower than the console: both edges with it, so the stopband starts at the device's own
  // Nyquist. The kernel keeps its length, so the narrower transition costs attenuation (~62 dB at 22.05 kHz).
  const Band b = band_of(m);
  const double pass = b.pass / scale, stop = b.stop / scale;
  const double atten = (stop - pass) * 2.285 * 2.0 * PI * (OutputFilter::TAPS - 1) + 7.95;
  return Design{(pass + stop) / 2.0, 0.1102 * (atten - 8.7)};
}

// The two channels' dot products with the kernel row between r0 and r1 (a of the way). By hand on NEON: GCC
// vectorised the plain loop across the wrong axis (de-interleaving loads and scalar sums, ~0.6 ms a frame on the
// RG DS Plus' A55).
inline void dot(const float* r0, const float* r1, float a, const float* xl, const float* xr, float& vl, float& vr) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#if defined(__aarch64__)
#define DS_FMA(acc, a, b) vfmaq_f32(acc, a, b)   // fused: one op, and the A55 has no fmul + fadd pair to wait on
#else
#define DS_FMA(acc, a, b) vmlaq_f32(acc, a, b)
#endif
  const float32x4_t va = vdupq_n_f32(a);
  // Two accumulators a channel: the in-order A55 would otherwise wait on each add before the next.
  float32x4_t sl0 = vdupq_n_f32(0.0f), sl1 = sl0, sr0 = sl0, sr1 = sl0;
  for (int j = 0; j < OutputFilter::TAPS; j += 8) {
    const float32x4_t k0 = vld1q_f32(r0 + j), k1 = vld1q_f32(r0 + j + 4);
    const float32x4_t c0 = DS_FMA(k0, vsubq_f32(vld1q_f32(r1 + j), k0), va);
    const float32x4_t c1 = DS_FMA(k1, vsubq_f32(vld1q_f32(r1 + j + 4), k1), va);
    sl0 = DS_FMA(sl0, c0, vld1q_f32(xl + j)); sl1 = DS_FMA(sl1, c1, vld1q_f32(xl + j + 4));
    sr0 = DS_FMA(sr0, c0, vld1q_f32(xr + j)); sr1 = DS_FMA(sr1, c1, vld1q_f32(xr + j + 4));
  }
#undef DS_FMA
  const float32x4_t sl = vaddq_f32(sl0, sl1), sr = vaddq_f32(sr0, sr1);
  const float32x2_t hl = vadd_f32(vget_low_f32(sl), vget_high_f32(sl)), hr = vadd_f32(vget_low_f32(sr), vget_high_f32(sr));
  const float32x2_t h = vpadd_f32(hl, hr);
  vl = vget_lane_f32(h, 0); vr = vget_lane_f32(h, 1);
#else
  float al = 0.0f, ar = 0.0f;
  for (int j = 0; j < OutputFilter::TAPS; ++j) {
    const float c = r0[j] + a * (r1[j] - r0[j]);
    al += c * xl[j]; ar += c * xr[j];
  }
  vl = al; vr = ar;
#endif
}

} // namespace

void OutputFilter::reset() {
  hist_l_.assign(TAPS, 0.0f);
  hist_r_.assign(TAPS, 0.0f);
  t_ = TAPS / 2 - 1;   // the first output's kernel starts at frame 0
  dc_x_[0] = dc_x_[1] = dc_y_[0] = dc_y_[1] = 0.0f;
}

void OutputFilter::design(Model model, double ratio) {
  const double scale = std::max(1.0, ratio);
  model_ = model; scale_ = scale; have_ = true;
  const Design d = design_of(model, scale);
  table_.assign(static_cast<size_t>(PHASES + 1) * TAPS, 0.0f);
  for (int p = 0; p <= PHASES; ++p) {
    const double f = static_cast<double>(p) / PHASES;
    float* row = &table_[static_cast<size_t>(p) * TAPS];
    double sum = 0.0;
    for (int j = 0; j < TAPS; ++j) sum += kernel(j - TAPS / 2 + 1 - f, d.fc, d.beta);
    for (int j = 0; j < TAPS; ++j) row[j] = static_cast<float>(kernel(j - TAPS / 2 + 1 - f, d.fc, d.beta) / sum);   // unity at DC
  }
}

double OutputFilter::response(double f) const {
  // The prototype sampled at 1/PHASES of a frame: the interpolator's response, in cycles per input frame.
  const Design d = design_of(model_, scale_ > 0.0 ? scale_ : 1.0);
  double re = 0.0, dc = 0.0;
  for (int n = -TAPS / 2 * PHASES; n <= TAPS / 2 * PHASES; ++n) {
    const double x = static_cast<double>(n) / PHASES, h = kernel(x, d.fc, d.beta);
    re += h * std::cos(2.0 * PI * f * x);
    dc += h;
  }
  return std::fabs(re / dc);
}

size_t OutputFilter::process(Model model, const s16* in, size_t n, double ratio, double out_rate, std::vector<s16>& out) {
  if (hist_l_.empty()) reset();
  // A new table for a new model, or a device rate the band has to narrow for (not for the trim's 0.5%).
  if (!have_ || model != model_ || std::fabs(std::max(1.0, ratio) - scale_) > 0.01) design(model, ratio);
  for (size_t i = 0; i < n; ++i) { hist_l_.push_back(in[i * 2]); hist_r_.push_back(in[i * 2 + 1]); }

  const float r = static_cast<float>(1.0 - 2.0 * PI * DC_HZ / out_rate);   // one-pole DC block
  const size_t m0 = out.size() / 2;
  // Room for every output this call can make, written by index and trimmed after.
  const double room = (static_cast<double>(hist_l_.size()) - t_) / ratio + 2.0;
  out.resize((m0 + static_cast<size_t>(std::max(0.0, room))) * 2);
  s16* o = out.data() + m0 * 2;
  float dxl = static_cast<float>(dc_x_[0]), dxr = static_cast<float>(dc_x_[1]);
  float dyl = static_cast<float>(dc_y_[0]), dyr = static_cast<float>(dc_y_[1]);
  size_t m = 0;
  for (;;) {
    const double fl = std::floor(t_);
    const size_t i = static_cast<size_t>(fl);
    if (i + TAPS / 2 >= hist_l_.size()) break;   // its last frame has not arrived
    const double pos = (t_ - fl) * PHASES;
    const int p = std::min(static_cast<int>(pos), PHASES - 1);
    const float a = static_cast<float>(pos - p);
    const float* r0 = &table_[static_cast<size_t>(p) * TAPS];
    const float* r1 = r0 + TAPS;
    const float* xl = &hist_l_[i + 1 - TAPS / 2];
    const float* xr = &hist_r_[i + 1 - TAPS / 2];
    float vl, vr;
    dot(r0, r1, a, xl, xr, vl, vr);
    const float yl = vl - dxl + r * dyl, yr = vr - dxr + r * dyr;
    dxl = vl; dyl = yl; dxr = vr; dyr = yr;
    o[m * 2]     = static_cast<s16>(std::clamp(yl + (yl < 0.0f ? -0.5f : 0.5f), -32768.0f, 32767.0f));
    o[m * 2 + 1] = static_cast<s16>(std::clamp(yr + (yr < 0.0f ? -0.5f : 0.5f), -32768.0f, 32767.0f));
    ++m;
    t_ += ratio;
  }
  out.resize((m0 + m) * 2);
  dc_x_[0] = dxl; dc_x_[1] = dxr; dc_y_[0] = dyl; dc_y_[1] = dyr;
  // Drop the frames no kernel reaches any more.
  const size_t keep_from = static_cast<size_t>(std::floor(t_)) + 1 - TAPS / 2;
  if (keep_from > 0 && keep_from <= hist_l_.size()) {
    hist_l_.erase(hist_l_.begin(), hist_l_.begin() + static_cast<std::ptrdiff_t>(keep_from));
    hist_r_.erase(hist_r_.begin(), hist_r_.begin() + static_cast<std::ptrdiff_t>(keep_from));
    t_ -= static_cast<double>(keep_from);
  }
  return m;
}

} // namespace ds::spu
