// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstdio>
#include <string>

namespace ds { struct NDS; }
namespace ds::input {

// One frame of player input: what the frontend hands the core before
// `run_frame`. Buttons are Io::Button bits; the pen is in screen pixels.
struct Frame {
  static constexpr int MIC_SAMPLES = 8;
  u16  buttons = 0;
  u8   x = 0, y = 0;
  bool down = false;
  bool lid = false;                 // hinge closed
  // Microphone, MIC_SAMPLES evenly spaced samples over the frame (~480 Hz):
  // enough for the games that measure loudness (blowing, shouting), which
  // is what a recorded scene needs to reproduce. A live session feeds the
  // core the capture at full rate and logs this decimation of it.
  s8   mic[MIC_SAMPLES] = {};
  bool silent() const { for (s8 m : mic) if (m) return false; return true; }
  bool operator==(const Frame& o) const {
    if (buttons != o.buttons || x != o.x || y != o.y || down != o.down || lid != o.lid) return false;
    for (int i = 0; i < MIC_SAMPLES; ++i) if (mic[i] != o.mic[i]) return false;
    return true;
  }
};

// Drives the registers from a frame. With `mic` the core samples that
// buffer (any rate, s16 mono) for the frame instead of the frame's own eight
// samples; the frame's eight are what gets logged either way.
void apply(NDS& nds, const Frame& f, const s16* mic = nullptr, size_t mic_count = 0);
// Decimates a capture buffer into a frame's mic samples (peak-preserving:
// each slot keeps the largest-magnitude sample of its span, so a short burst
// still registers).
void decimate_mic(Frame& f, const s16* mic, size_t mic_count);

// Input log file: a played session as one record per frame, so a scene can
// be replayed headlessly as a benchmark or a perf run. The emulator is
// deterministic given its inputs, so a replay reproduces the session frame
// for frame provided the ROM, BIOS and battery save match.
//
// Layout: 16-byte header ("DSIN", version u32, frame count u32 — written
// on close, zero while recording — and 4 bytes reserved), then one record
// per frame. Version 2 records are 16 bytes: buttons u16, x u8, y u8,
// flags u8 (bit 0 = pen down, bit 1 = lid closed), 3 reserved, then the
// eight s8 mic samples. Version 1 records were the first 8 bytes and are
// still read (silent mic, lid open).
class Log {
public:
  ~Log() { close(); }
  bool open_write(const std::string& path);
  bool open_read(const std::string& path);
  void close();

  void write(const Frame& f);
  bool read(Frame& f);            // false at the end
  u32  frames() const { return count_; }
  bool writing() const { return f_ && write_; }
  bool reading() const { return f_ && !write_; }

private:
  FILE* f_ = nullptr;
  bool  write_ = false;
  u32   count_ = 0;
  size_t record_ = 16;
};

} // namespace ds::input
