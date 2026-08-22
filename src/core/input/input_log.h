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
  u16  buttons = 0;
  u8   x = 0, y = 0;
  bool down = false;
  bool operator==(const Frame& o) const { return buttons == o.buttons && x == o.x && y == o.y && down == o.down; }
};

void apply(NDS& nds, const Frame& f);

// Input log file: a played session as one record per frame, so a scene can
// be replayed headlessly as a benchmark or a perf run. The emulator is
// deterministic given its inputs, so a replay reproduces the session frame
// for frame provided the ROM, BIOS and battery save match.
//
// Layout: 16-byte header ("DSIN", version u32, frame count u32 — written
// on close, zero while recording — and 4 bytes reserved), then 8 bytes per
// frame: buttons u16, x u8, y u8, flags u8 (bit 0 = pen down), 3 reserved.
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
};

} // namespace ds::input
