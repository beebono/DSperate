// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/spu/spu.h"

#include <SDL2/SDL.h>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

// Audio output and the frame pacer.
//
// The device is opened at the SPU's own 32768 Hz and SDL converts to whatever
// the hardware wants, so the core never resamples. Samples are queued rather
// than pulled from a callback: the SPU already buffers a ring, and queueing
// keeps the whole frontend single-threaded.
//
// The queue is also the clock. Emulating a frame produces a fixed 547-odd
// samples of audio, so holding the queue near a target depth paces the
// emulator at exactly the DS's frame rate without a timer.
class Audio {
public:
  bool open();
  void close();
  bool active() const { return dev_ != 0; }

  void push(NDS& nds);      // drain the SPU ring into the queue

  // Microphone: the default capture device at the SPU rate, mono. Opened
  // separately so --no-audio still records. capture() hands back everything
  // captured since the last call (at most a few frames' worth: a backlog is
  // dropped, the game wants what is being said now, not what was).
  bool open_capture();
  const std::vector<s16>& capture();
  bool capturing() const { return cap_ != 0; }
  void pace();              // sleep while the queue is above the target depth
  double queued_frames() const;   // how much audio is buffered, in frames

private:
  static constexpr int TARGET_FRAMES = 3;    // ~50 ms of slack
  static constexpr int STALLED_FRAMES = 30;  // a queue this deep means nothing is playing
  SDL_AudioDeviceID dev_ = 0, cap_ = 0;
  std::vector<s16> mic_;
  u32 frame_bytes_ = 0;
  bool stalled_ = false;
};

} // namespace ds::sdl
