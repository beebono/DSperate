// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <SDL2/SDL.h>

#if defined(__linux__)
#include <cerrno>
#include <ctime>
#include <sys/prctl.h>
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
  explicit Pacer(double period_ns) {
#if defined(__linux__)
    // The kernel rounds a timer up by the thread's slack -- 50 us by default,
    // which is most of the error a frame wait sees. It costs nothing to ask
    // for none (an RT thread is already given none, so this is for the
    // ordinary case). Per-thread, and this runs on the emulation thread.
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
#endif
    set_period_ns(period_ns);
    reset();
  }

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

  // What the spin is costing, in microseconds a frame, averaged since the
  // last call. For the statistics line; the margin tunes itself without it.
  double spin_us() {
    const double v = spin_n_ ? spin_ticks_ / ticks_per_ns() / 1e3 / spin_n_ : 0.0;
    spin_ticks_ = 0; spin_n_ = 0;
    return v;
  }

private:
  // A sleep does not end when it was asked to: the kernel wakes the thread
  // late, by its timer slack plus whatever else wanted the core. Measured on
  // an idle desktop, ordinary priority: 60 us late at the median but 430 us
  // at the 99th percentile, and the two do not move together -- a constant
  // spin margin is therefore either 30x too large most frames or too small
  // on the ones that matter.
  //
  // So the margin is what this thread has actually been seen to overshoot
  // by, and it is learned: it jumps straight to any lateness bigger than it
  // has, and decays slowly (1/512 a frame, ~2 s to halve) when frames come
  // back on time. The asymmetry is the point -- being early costs a few
  // microseconds of spin, being late costs a missed frame -- and it means
  // the number tunes itself per device and per scheduling policy rather
  // than being one guess for a desktop and an RK3566 alike. An RT thread is
  // given no timer slack at all, so on a device that runs under SCHED_RR
  // this settles near zero and the spin all but disappears.
  static constexpr double MARGIN_DECAY = 1.0 - 1.0 / 512.0;
  static constexpr double MARGIN_MAX_NS = 2e6;    // never hand more than 2 ms to the spin

  static double ticks_per_ns() {
    static const double v = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;
    return v;
  }

  void sleep_until(Uint64 deadline, Uint64 now) {
    const double margin = margin_ticks_;
    const double left = static_cast<double>(deadline - now);
    if (left > margin) {
      const double nap_ns = (left - margin) / ticks_per_ns();
      const Uint64 asked = deadline - static_cast<Uint64>(margin);
#if defined(__linux__)
      // Absolute rather than relative: a signal restarts the wait against
      // the same instant instead of the remainder, so a stray SIGALRM can
      // neither shorten nor lengthen the frame. The deadline is built from
      // the monotonic clock read here, which keeps this independent of
      // whichever clock SDL's counter is on.
      timespec mono{};
      clock_gettime(CLOCK_MONOTONIC, &mono);
      long long end = mono.tv_sec * 1'000'000'000LL + mono.tv_nsec + static_cast<long long>(nap_ns);
      timespec until{static_cast<time_t>(end / 1'000'000'000LL), static_cast<long>(end % 1'000'000'000LL)};
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
#else
      const double nap_ms = nap_ns / 1e6;
      if (nap_ms > 1.0) SDL_Delay(static_cast<Uint32>(nap_ms));
#endif
      // What the sleep actually cost against what was asked for. Only a
      // real sleep teaches the margin anything; a frame that spun the whole
      // way has nothing to say about the kernel's wakeups.
      const Uint64 woke = SDL_GetPerformanceCounter();
      const double late = static_cast<double>(woke) - static_cast<double>(asked);
      const double cap = MARGIN_MAX_NS * ticks_per_ns();
      margin_ticks_ = late > margin_ticks_ ? (late < cap ? late : cap) : margin_ticks_ * MARGIN_DECAY;
      if (woke >= deadline) return;      // woke past the deadline: nothing left to spin
      spin_ticks_ += static_cast<double>(deadline - woke);
    }
    ++spin_n_;
    while (SDL_GetPerformanceCounter() < deadline) {}
  }

  double period_ = 0;        // in performance-counter ticks
  Uint64 next_ = 0;
  double margin_ticks_ = 0;  // learned; starts at zero and grows into the first few frames
  double spin_ticks_ = 0;    // what the spin has cost since the last spin_us()
  unsigned spin_n_ = 0;
};

} // namespace ds::sdl
