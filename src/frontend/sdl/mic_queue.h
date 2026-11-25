// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ds::sdl {

// Turns the capture device's bursts into one emulated frame of samples at a
// time. The core spreads whatever a frame gets across the whole frame, so a
// short frame plays slow: the queue keeps a reserve of the largest burst seen
// (ALSA on the RG DS Plus delivers 1024 samples every other frame) and hands
// out a steady count, nudged by at most MAX_TRIM to hold the reserve where it
// should be against the drift between the capture clock and the frame rate.
// Running dry hands out what is left and primes again; a backlog past the
// reserve (a stall) is dropped so it does not turn into latency.
class MicQueue {
public:
  static constexpr double MAX_TRIM = 0.02;     // most a frame's count moves off nominal
  static constexpr double GAIN = 0.04;         // fraction of the reserve's error corrected a frame
  static constexpr double SMOOTH = 0.05;       // level average, across the burst pattern
  static constexpr double MAX_BURST = 4;       // largest delivery, in frames, taken as the device's period

  explicit MicQueue(double per_frame) : per_frame_(per_frame) {}

  void reset() { q_.clear(); primed_ = false; burst_ = 0; avg_ = 0; frac_ = 0; }

  // Takes this frame's capture, writes the frame's samples to `out` (none while priming).
  void frame(const s16* in, size_t n, std::vector<s16>& out) {
    out.clear();
    q_.insert(q_.end(), in, in + n);
    // A delivery of more than MAX_BURST frames is a backlog after a stall, not the device's period.
    if (static_cast<double>(n) <= per_frame_ * MAX_BURST) burst_ = std::max(static_cast<double>(n), burst_ * 0.995);
    const double target = this->target();
    if (!primed_) {
      if (q_.empty() || static_cast<double>(q_.size()) < target) return;
      primed_ = true; avg_ = target; frac_ = 0;   // the level just after a burst is above its mean
    }
    if (static_cast<double>(q_.size()) > target + 4 * per_frame_) {
      q_.erase(q_.begin(), q_.end() - static_cast<std::ptrdiff_t>(target));
      avg_ = target;
    }
    avg_ += (static_cast<double>(q_.size()) - avg_) * SMOOTH;
    const double trim = std::clamp((avg_ - target) * GAIN, -per_frame_ * MAX_TRIM, per_frame_ * MAX_TRIM);
    frac_ += per_frame_ + trim;
    size_t take = static_cast<size_t>(frac_);
    frac_ -= static_cast<double>(take);
    if (take >= q_.size()) { take = q_.size(); primed_ = false; ++underruns_; }
    out.assign(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(take));
    q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(take));
  }

  // The level to hold before a frame's take: a whole burst plus a frame's margin, for a
  // capture period that comes a frame late (seen on the RG DS Plus about once a minute
  // with half a frame).
  double target() const { return burst_ + per_frame_; }
  size_t level() const { return q_.size(); }
  bool primed() const { return primed_; }
  unsigned underruns() const { return underruns_; }

private:
  double per_frame_;
  std::vector<s16> q_;
  bool primed_ = false;
  double burst_ = 0, avg_ = 0, frac_ = 0;
  unsigned underruns_ = 0;
};

} // namespace ds::sdl
