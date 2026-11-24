// SPDX-License-Identifier: GPL-3.0-or-later
// spu::OutputFilter (audio.hw_filter): each model's band where its source puts
// it, images gone from a real resample, a slower device's band narrowed, DC
// taken out.
#include "check.h"
#include "core/spu/output_filter.h"

#include <cmath>
#include <cstdio>
#include <vector>

using ds::spu::OutputFilter;
using ds::s16;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double DS_RATE = 67027964.0 / 2048.0;   // 32728.5 Hz
constexpr double DSI_HI_RATE = 67027964.0 / 1408.0;   // 47605.1 Hz

double db(double g) { return 20.0 * std::log10(g); }

// Level of `hz` in a mono track (left channel), dB relative to full scale, by correlation over whole cycles.
double level_db(const std::vector<s16>& st, double rate, double hz, size_t skip) {
  double re = 0.0, im = 0.0;
  const size_t n = st.size() / 2;
  for (size_t i = skip; i < n; ++i) {
    const double w = 2.0 * PI * hz * static_cast<double>(i) / rate;
    re += st[i * 2] * std::cos(w); im += st[i * 2] * std::sin(w);
  }
  const double amp = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n - skip);
  return db(amp / 32767.0 + 1e-12);
}

std::vector<s16> resample_tone(OutputFilter::Model m, double in_rate, double hz, double out_rate, double amp = 16000.0, double dc = 0.0) {
  OutputFilter f;
  f.reset();
  std::vector<s16> in, out;
  const size_t frames = static_cast<size_t>(in_rate);   // one second
  for (size_t i = 0; i < frames; ++i) {
    const s16 v = static_cast<s16>(std::lround(dc + amp * std::sin(2.0 * PI * hz * static_cast<double>(i) / in_rate)));
    in.push_back(v); in.push_back(v);
  }
  // In pieces, as the frontend hands them over.
  for (size_t at = 0; at < frames; at += 700) {
    const size_t n = std::min<size_t>(700, frames - at);
    f.process(m, &in[at * 2], n, in_rate / out_rate, out_rate, out);
  }
  return out;
}

} // namespace

int main() {
  OutputFilter f;

  // DS: flat through the passband, nothing from its Nyquist up.
  f.design(OutputFilter::Model::Ds, 0.68);
  CHECK(std::fabs(db(f.response(0.10))) < 0.01);
  CHECK(std::fabs(db(f.response(0.42))) < 0.05);
  CHECK(db(f.response(0.50)) < -70.0);
  CHECK(db(f.response(0.60)) < -70.0);

  // DSi: TSC2117 filter A, +-0.015 dB to 0.45 fs and -65 dB from 0.55 fs (ours keeps inside both).
  f.design(OutputFilter::Model::Dsi, 0.68);
  CHECK(std::fabs(db(f.response(0.45))) < 0.015);
  CHECK(db(f.response(0.55)) < -65.0);

  // A device slower than the console: the band narrows to the device's Nyquist (with less attenuation: the kernel
  // keeps its length).
  f.design(OutputFilter::Model::Ds, DS_RATE / 22050.0);
  std::printf("ds into 22.05 kHz, at its Nyquist: %.1f dB\n", db(f.response(0.5 * 22050.0 / DS_RATE)));
  CHECK(db(f.response(0.5 * 22050.0 / DS_RATE)) < -50.0);

  // A real resample, DS rate to 48 kHz: a 1 kHz tone keeps its level, a 10 kHz tone's image (at 22.7 kHz) is gone,
  // and DC added under it is taken out.
  {
    const std::vector<s16> out = resample_tone(OutputFilter::Model::Ds, DS_RATE, 1000.0, 48000.0);
    const double l = level_db(out, 48000.0, 1000.0, 4800);
    std::printf("ds 1 kHz: %.3f dB\n", l);
    CHECK(std::fabs(l - db(16000.0 / 32767.0)) < 0.05);
  }
  {
    const std::vector<s16> out = resample_tone(OutputFilter::Model::Ds, DS_RATE, 10000.0, 48000.0);
    const double tone = level_db(out, 48000.0, 10000.0, 4800), image = level_db(out, 48000.0, DS_RATE - 10000.0, 4800);
    std::printf("ds 10 kHz: %.2f dB, image %.2f dB\n", tone, image);
    CHECK(tone - image > 60.0);
  }
  {
    const std::vector<s16> out = resample_tone(OutputFilter::Model::Ds, DS_RATE, 1000.0, 48000.0, 8000.0, 6000.0);
    double mean = 0.0; size_t k = 0;
    for (size_t i = 48000 / 2; i < out.size() / 2; ++i) { mean += out[i * 2]; ++k; }   // past the block's settling
    mean /= static_cast<double>(k);
    std::printf("dc after the block: %.2f\n", mean);
    CHECK(std::fabs(mean) < 50.0);
  }
  // DSi at its 47.6 kHz rate into 48 kHz: 20 kHz passes (0.42 fs).
  {
    const std::vector<s16> out = resample_tone(OutputFilter::Model::Dsi, DSI_HI_RATE, 20000.0, 48000.0);
    const double l = level_db(out, 48000.0, 20000.0, 4800);
    std::printf("dsi 20 kHz at 47.6: %.3f dB\n", l);
    CHECK(std::fabs(l - db(16000.0 / 32767.0)) < 0.05);
  }
  std::printf("output filter: ok\n");
  return 0;
}
