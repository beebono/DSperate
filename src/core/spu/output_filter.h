// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The console's own output stage, as far as it is documented (audio.hw_filter):
// a band-limited resampler from the SPU's rate to the device's, shaped like
// the console that would have played it, and a DC block for the coupling
// capacitor every analog output has.
//
//   DS / DS Lite  The SPU's 10-bit PWM at 32.73 kHz (GBATEK, "DS Sound"):
//                 nothing above half that rate can come out. Passband to
//                 0.42 fs, stopband from 0.5 fs. The analog smoothing after
//                 the PWM is not documented anywhere, so it is not modelled.
//                 (The 10 bits are Spu::set_ten_bit, in the core.)
//   DSi / DSi XL  The AIC3000D codec, a TI TSC2117 (GBATEK, "DSi TSC"), left
//                 at its reset processing block PRB_P1 by the init GBATEK
//                 lists: interpolation filter A, flat (+-0.015 dB) to 0.45 fs,
//                 -65 dB from 0.55 fs (TSC2117 datasheet SLAS550B, 5.6.1.4.1),
//                 at 32.73 or 47.61 kHz.
//
// A polyphase windowed sinc (Kaiser), TAPS input frames wide; the cutoff
// narrows with the ratio when the device's rate is lower than the console's,
// so nothing above the device's own Nyquist folds back. The DC block's corner
// (DC_HZ) is a choice, not a measurement: low enough to leave every note.
#pragma once
#include "core/types.h"

#include <cstddef>
#include <vector>

namespace ds::spu {

class OutputFilter {
public:
  enum class Model : u8 { Ds, Dsi };
  static constexpr int TAPS = 64;          // input frames under the kernel (even): the transitions above at >= 80 dB
  static constexpr int PHASES = 64;        // kernel rows between two input frames: 16 KiB of table, in an A55 L1
  static constexpr double DC_HZ = 10.0;    // the DC block's corner

  // Starts again from silence (a model change, a pause, a lost device).
  void reset();
  // Resamples `n` stereo frames at `ratio` input frames per output frame,
  // appending the output frames to `out` (2 s16 each). Returns how many.
  size_t process(Model model, const s16* in, size_t n, double ratio, double out_rate, std::vector<s16>& out);

  // The kernel's response at `f` (cycles per input frame), linear gain: for the tests.
  double response(double f) const;
  // Design one table now (tests); process() designs its own as it needs to.
  void design(Model model, double ratio);

private:
  Model  model_ = Model::Ds;
  double scale_ = 0.0;                  // the band's narrowing the table was made for (>= 1)
  bool   have_ = false;
  std::vector<float> table_;            // (PHASES + 1) rows of TAPS
  std::vector<float> hist_l_, hist_r_;  // input frames, the oldest first
  double t_ = 0.0;                      // the next output's time, in input frames into hist_
  float  dc_x_[2] = {}, dc_y_[2] = {};   // DC block state
};

} // namespace ds::spu
