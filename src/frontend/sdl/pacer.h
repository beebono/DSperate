// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <SDL2/SDL.h>

#if defined(__linux__)
#include <cerrno>
#include <ctime>
#endif

namespace ds::sdl {

// The frame limiter: the emulator's clock.
//
// It used to be the audio queue -- a frame produces a fixed 547-odd samples,
// so holding the queue at a depth held the emulator at the rate the *sound
// card* consumed them. That paced well and cost nothing, but the clock was
// the daemon's rather than ours: no speed control, latency pinned at the
// queue depth, and a device that accepted samples without playing them had
// to be detected and worked around before it hung the machine. This is the
// wall clock instead, and the period is a knob.
//
// Deadlines accumulate rather than being measured from the end of each
// frame, so a frame that runs long is paid for by the next one rather than
// pushing the whole session late. Debt is capped at one frame: a title that
// alternates heavy and light (Spirit Tracks' intro, Golden Sun's title:
// 18 ms then 12 ms) is on time over the pair, and dropping the debt after
// the heavy frame made the light one sleep the difference away -- 57.6 fps
// out of 15 ms of work.
class Pacer {
public:
  // The nominal period, in nanoseconds -- CYCLES_PER_FRAME / ARM9_CLOCK_HZ,
  // i.e. 59.8261 Hz, unless something asks for another rate.
  explicit Pacer(double period_ns) { set_period_ns(period_ns); reset(); }

  void set_period_ns(double ns) { period_ = ns * ticks_per_ns(); }
  double period_ns() const { return period_ / ticks_per_ns(); }

  // Start again from now: the deadline is one period away. For every place
  // where wall time and emulated time have just been cut apart -- unpause,
  // a state load, a speed change -- so the gap is not repaid as a burst.
  void reset() { next_ = SDL_GetPerformanceCounter(); }

  // This frame's deadline, in SDL performance-counter ticks. Read by the
  // DS_WIFI_SLICE path, which spreads a frame's emulation across its period.
  Uint64 next() const { return next_; }

  // Advance to the next deadline and sleep until it. `scale` runs the clock
  // faster or slower than the period: 2.0 is double speed, 0.5 half.
  void wait(double scale = 1.0) {
    next_ += static_cast<Uint64>(scale > 0.0 ? period_ / scale : period_);
    const Uint64 now = SDL_GetPerformanceCounter();
    if (now >= next_) {
      // Behind. Carry at most one frame of debt (see above).
      const Uint64 budget = static_cast<Uint64>(period_);
      if (now - next_ > budget) next_ = now - budget;
      return;
    }
    sleep_until(next_, now);
  }

private:
  // How much of the wait is spun rather than slept. SDL_Delay() rounds up to
  // whole milliseconds, which on a 16.7 ms frame is 6 % of the period -- no
  // matter while the audio queue was absorbing it, a visible judder now that
  // this is the clock. nanosleep() is finer but still only as good as the
  // timer slack it wakes with, so the last stretch is spun.
  static constexpr double SPIN_NS = 400'000.0;   // 0.4 ms

  static double ticks_per_ns() {
    static const double v = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;
    return v;
  }

  static void sleep_until(Uint64 deadline, Uint64 now) {
#if defined(__linux__)
    const double left_ns = (deadline - now) / ticks_per_ns();
    if (left_ns > SPIN_NS) {
      const double nap = left_ns - SPIN_NS;
      timespec ts{static_cast<time_t>(nap / 1e9), static_cast<long>(nap - static_cast<long long>(nap / 1e9) * 1e9)};
      // Restarted on a signal: a stray SIGALRM must not shorten the frame.
      while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
    }
#else
    const double left_ms = (deadline - now) / ticks_per_ns() / 1e6;
    if (left_ms > 1.0) SDL_Delay(static_cast<Uint32>(left_ms));
#endif
    while (SDL_GetPerformanceCounter() < deadline) {}
  }

  double period_ = 0;    // in performance-counter ticks
  Uint64 next_ = 0;
};

} // namespace ds::sdl
