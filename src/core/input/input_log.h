// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstdio>
#include <string>

namespace ds { struct NDS; }
namespace ds::input {

// One frame of player input, handed to the core before `run_frame`. Buttons are Io::Button
// bits; the pen is in screen pixels.
struct Frame {
  static constexpr int MIC_SAMPLES = 8;
  u16  buttons = 0;
  u8   x = 0, y = 0;
  bool down = false;
  bool lid = false;                 // hinge closed
  // Analog patch channel (shocoman-style CPAD patches): two sticks in SDL
  // range, plus ZL/ZR. Zero unless the frontend fills them; the core only
  // forwards them to guest RAM when NDS::analog_patch is set.
  s16  cpad_x = 0, cpad_y = 0;       // main stick (Circle Pad)
  s16  cstick_x = 0, cstick_y = 0;   // second stick (C-Stick / nub)
  bool zl = false, zr = false;
  // Guitar Grip frets held (slot2::GuitarGrip::Fret bits); reach the game
  // only while the grip is in slot 2.
  u8   frets = 0;
  // MIC_SAMPLES evenly spaced samples over the frame (~480 Hz); enough for loudness detection.
  s8   mic[MIC_SAMPLES] = {};
  bool silent() const { for (s8 m : mic) if (m) return false; return true; }
  bool operator==(const Frame& o) const {
    if (buttons != o.buttons || x != o.x || y != o.y || down != o.down || lid != o.lid) return false;
    if (cpad_x != o.cpad_x || cpad_y != o.cpad_y || cstick_x != o.cstick_x || cstick_y != o.cstick_y) return false;
    if (zl != o.zl || zr != o.zr || frets != o.frets) return false;
    for (int i = 0; i < MIC_SAMPLES; ++i) if (mic[i] != o.mic[i]) return false;
    return true;
  }
};

// Drives the registers from a frame. With `mic`, the core samples that buffer (any rate, s16
// mono) instead of the frame's own eight; the frame's eight are logged either way.
void apply(NDS& nds, const Frame& f, const s16* mic = nullptr, size_t mic_count = 0);

// Zeroes the analog channel, for when nothing is feeding it: a hook must not
// keep reading the last stick values, or a state's.
void clear_analog(NDS& nds);
// Decimates a capture buffer into a frame's mic samples (peak-preserving per slot).
void decimate_mic(Frame& f, const s16* mic, size_t mic_count);

// Input log file: one record per frame, replayable headlessly (deterministic given ROM, BIOS,
// battery save).
//
// Layout: 16-byte header ("DSIN", version u32, frame count u32 written on close, start frame
// u32: the machine's frame count when the log began, 0 from boot or in older logs),
// then one record per frame. Version 2 records: buttons u16, x u8, y u8, flags u8 (bit 0 pen
// down, bit 1 lid closed), Guitar Grip frets u8 (zero in older logs), 2 reserved, eight s8
// mic samples. Version 1 records are the first
// 8 bytes only (silent mic, lid open). Version 3 records are 24 bytes: the version 2 record
// (flags gain bit 2 ZL, bit 3 ZR) followed by cpad_x, cpad_y, cstick_x, cstick_y as s16.
class Log {
public:
  ~Log() { close(); }
  bool open_write(const std::string& path);
  bool open_read(const std::string& path);
  void close();

  void write(const Frame& f);
  bool read(Frame& f);            // false at the end
  u32  frames() const { return count_; }
  // A recording begun from a save state starts at that state's frame, not
  // at boot; set before closing (it is written with the frame count).
  void set_start(u32 frame) { start_ = frame; }
  u32  start() const { return start_; }
  // Replay resuming from a state at `frame`: skips the records before it.
  // False when the log begins after `frame` (it was made from a later state).
  bool wind_to(u64 frame);
  bool writing() const { return f_ && write_; }
  bool reading() const { return f_ && !write_; }

private:
  FILE* f_ = nullptr;
  bool  write_ = false;
  u32   start_ = 0;
  u32   count_ = 0;
  size_t record_ = 16;
};

} // namespace ds::input
