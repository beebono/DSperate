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
// The device is opened at its own rate and the SPU's 32768 Hz stream is
// resampled here on the way in (linear, two taps: a 32.768 -> 48 kHz step on
// a DS mix is well inside what the DAC's own filtering hides). Resampling in
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
  u32  in_rate_ = spu::Spu::SAMPLE_RATE;   // the SPU's output rate, which a DSi title can change
  int  volume_ = 100;
  bool muted_ = false;
};

} // namespace ds::sdl
