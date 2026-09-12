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
// leaves the queue a latency buffer and nothing else -- and leaves the two
// rates free to drift. Phase 1 only bounds the damage: a queue that has run
// past MAX_FRAMES drops whole frames of audio until it is back at the
// target. Steering the resampler to hold the depth instead (dynamic rate
// control -- docs/frame-pacing-scoping.md, phase 2) is what makes this
// inaudible; until then a slow drift costs an occasional dropped frame of
// sound, and the other direction runs the queue dry.
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
  void set_volume(int percent);   // 0..100
  int  volume() const { return volume_; }
  void set_muted(bool m) { muted_ = m; }
  bool muted() const { return muted_; }
  void pause(bool p);       // stop the device and drop what is queued
  void clear() { if (dev_) SDL_ClearQueuedAudio(dev_); }

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
  static constexpr int MAX_FRAMES = 6;       // drifted this far ahead: drop back to the target
  SDL_AudioDeviceID dev_ = 0, cap_ = 0;
  std::vector<s16> mic_;
  u32 frame_bytes_ = 0;
  u32 rate_ = spu::Spu::SAMPLE_RATE;     // the device's rate
  // Resampler state: the previous input frame and the output phase within
  // the current input step, 16.16.
  s16 prev_l_ = 0, prev_r_ = 0;
  u32 phase_ = 0;
  std::vector<s16> out_;
  bool over_ = false;    // the queue has drifted past MAX_FRAMES; dropping until it is back at the target
  int  volume_ = 100;
  bool muted_ = false;
};

} // namespace ds::sdl
