// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/spu/spu.h"

#include <SDL2/SDL.h>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

// Audio output.
//
// The device is opened at its own rate and the SPU's stream is resampled
// here on the way in (linear, two taps: a 32.7 -> 48 kHz step on a DS mix is
// well inside what the DAC's own filtering hides). That input rate is
// 32728.5 Hz, not the 32768 the DS rate is named after, and it is read live
// from the SPU because a DSi title can move it to 47605.1: the ratio is the
// one thing here that has to be exact, and a nominal rate 1207 ppm off left
// the rate control correcting our own constant for whole runs. See
// Spu::output_rate_hz() and docs/frame-pacing-scoping.md. Resampling in
// the sound daemon instead measured ~1 ms a frame on the RG DS (PipeWire's
// clock is locked to 48 kHz there) -- `native_rate` false restores that, the
// device then being asked for 32768 Hz. Samples are queued rather than pulled
// from a callback: the SPU already buffers a ring, and queueing keeps the
// whole frontend single-threaded.
//
// The queue used to be the clock: a frame produces a fixed 547-odd samples,
// so holding the queue at a depth held the emulator at the rate the sound
// card consumed them. The frame limiter (pacer.h) is the clock now, which
// leaves the queue a latency buffer -- and leaves two clocks free to drift
// apart. The limiter runs at the DS's 59.8261 Hz; the card consumes at
// whatever its crystal actually does, typically tens to hundreds of ppm off
// nominal. Left alone the queue walks to empty or to the ceiling within
// minutes, and either end is audible.
//
// So the resampler is steered: the queue depth is measured every frame and
// the step is nudged by a fraction of a percent to bring it back to the
// target. This is the standard fix (dynamic rate control), and the authority
// it needs -- a few tenths of a percent, a handful of cents of pitch -- is
// well under what anyone can hear on a DS mix. See
// docs/frame-pacing-scoping.md.
//
// The controller owns ordinary drift. Dropping whole frames is left for what
// it cannot answer for: fast forward (the speakers cannot play 2x), and a
// device that stops consuming altogether, which shows up as a queue past
// MAX_FRAMES and is capped rather than steered.
class Audio {
public:
  bool open(bool native_rate = true);
  u32  rate() const { return rate_; }
  void close();
  bool active() const { return dev_ != 0; }

  // Drains the SPU ring into the queue. With `drop`, whole frames are
  // discarded once the queue is at its target depth (fast forward: the
  // speakers cannot keep up, so play the newest and stay in sync). Whole
  // frames are dropped without it too, once the queue has drifted past
  // MAX_FRAMES.
  void push(NDS& nds, bool drop = false);
  // How fast the machine is running against the console's own rate: the
  // frame limiter's rate over 59.8261 Hz, times emu.speed. The SPU produces
  // a frame's samples however fast the frame runs, so at half speed half as
  // much audio arrives per second of wall clock and the resampler has to
  // stretch it to fill the device -- which is to say the sound slows and
  // drops in pitch, as it would on a console someone had slowed down. The
  // rate control cannot do this: a few tenths of a percent of trim against a
  // factor of two is the difference between a correction and a resampling.
  //
  // Fast forward is not this. It drops whole frames instead, because 2x of
  // pitched-up audio is not what anyone wants out of a fast forward.
  void set_speed(double factor);
  // The depth the controller holds the queue at, in frames of audio: the
  // output latency, and the slack a late frame has before the queue runs
  // dry. [audio] latency_frames.
  void set_latency_frames(int frames);
  int  latency_frames() const { return target_frames_; }
  // What the controller is doing to the sample rate right now, in parts per
  // million (positive: playing out faster than nominal to shed a deep
  // queue). For the statistics line -- a number that sits at one end of its
  // range means the drift is larger than the controller can answer.
  double rate_trim_ppm() const { return trim_ * 1e6; }
  // What the device actually gave us, which is not always what was asked
  // for: a daemon-backed device picks its own period. The queue target has
  // to clear this -- the device drains in whole chunks of it, so a target
  // under one chunk is a depth the controller can never hold.
  u32    device_samples() const { return dev_samples_; }
  double device_buffer_frames() const;   // that chunk, in frames of audio
  double input_rate() const { return in_rate_; }   // the SPU's, live (DSi: 47605.1)

  // Output statistics since the last reset. The controller's own inputs,
  // gathered where they are already measured rather than by asking SDL
  // again from the frame loop.
  //
  // `dry` is the underrun signal: the queue was empty when the frame looked
  // at it, so the device has played everything it had and anything it plays
  // before the next push is silence. There is no callback to catch a real
  // underrun in -- a queued device just goes quiet -- so this is the closest
  // thing to one, and it is what an automatic buffer size has to grow on.
  struct Stats {
    u64    frames = 0;        // frames the queue was measured on
    u64    dry = 0;           // ... and found empty
    u64    under_half = 0;    // ... and found under half a frame
    u64    under_one = 0;
    u64    dropped = 0;       // frames sent to drain() instead of the device
    double min_depth = 1e9;   // shallowest measurement, in frames
    double max_depth = 0.0;
  };
  const Stats& stats() const { return stats_; }
  void reset_stats() { stats_ = Stats{}; }

  void set_volume(int percent);   // 0..100
  int  volume() const { return volume_; }
  void set_muted(bool m) { muted_ = m; }
  bool muted() const { return muted_; }
  void pause(bool p);       // stop the device and drop what is queued
  void clear() { if (dev_) SDL_ClearQueuedAudio(dev_); depth_ = -1.0; trim_ = 0.0; }

  // Microphone: the default capture device at the SPU rate, mono. Opened
  // separately so --no-audio still records. capture() hands back everything
  // captured since the last call (at most a few frames' worth: a backlog is
  // dropped, the game wants what is being said now, not what was).
  bool open_capture();
  const std::vector<s16>& capture();
  bool capturing() const { return cap_ != 0; }
  double queued_frames() const;   // how much audio is buffered, in frames

private:
  static constexpr int TARGET_FRAMES = 3;    // ~50 ms of slack
  // Past this the queue is not drifting, it is not being consumed at all (a
  // dead daemon, a device the session lost): the controller cannot fix that
  // and would wind itself to its limit trying, so whole frames go instead.
  static constexpr int MAX_FRAMES = 8;
  // Controller: a proportional step on the depth error, in frames. 1 frame
  // of error asks for 0.2 %, which closes that frame of error in about 8
  // seconds; the clamp is what a DS mix can take without anyone hearing it
  // (0.5 % is ~8 cents). Steady-state offset is the drift divided by the
  // gain -- 100 ppm leaves the queue 0.05 frames off target, which is why
  // there is no integral term to wind up.
  static constexpr double DRC_GAIN = 0.002;      // per frame of depth error
  static constexpr double DRC_CLAMP = 0.005;     // +/- 0.5 %
  static constexpr double DRC_SMOOTH = 1.0 / 32.0;   // depth EMA, ~0.5 s
  SDL_AudioDeviceID dev_ = 0, cap_ = 0;
  std::vector<s16> mic_;
  u32 frame_bytes_ = 0;
  u32 dev_samples_ = 0;     // got.samples: the device's own period
  Stats stats_;
  u32 rate_ = spu::Spu::SAMPLE_RATE;     // the device's rate
  // Resampler state: the previous input frame and the output phase within
  // the current input step, 16.16.
  s16 prev_l_ = 0, prev_r_ = 0;
  u32 phase_ = 0;
  std::vector<s16> out_;
  bool over_ = false;    // the queue is not being consumed; dropping until it is back at the target
  int  target_frames_ = TARGET_FRAMES;
  double speed_ = 1.0;   // wall-clock rate against the console's own
  double depth_ = -1.0;  // smoothed queue depth in frames; < 0 until the first measurement
  double trim_ = 0.0;    // the controller's correction, as a fraction of the nominal rate
  // The SPU's real output rate, which a DSi title can change. Not an integer:
  // it is 32728.5 Hz on a DS, and rounding it to 32728 would leave 15 ppm of
  // the 1207 this is here to stop paying.
  double in_rate_ = 0.0;   // 0 until the first push reads it
  int  volume_ = 100;
  bool muted_ = false;
};

} // namespace ds::sdl
