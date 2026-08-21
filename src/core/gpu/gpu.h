// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds { struct NDS; }

namespace ds::gpu {

// Display timing plus (eventually) the 2D engines and the 3D pipeline.
// Planned as a mask-plane deferred scanline compositor with statically
// specialised kernels and NEON beside portable C++ (docs/ARCHITECTURE.md §5).
class Gpu {
public:
  explicit Gpu(NDS& nds) : nds_(nds) {}
  void reset();

  // Scheduler callbacks.
  void on_scanline_start();   // VCOUNT advance, VBlank/VCount flags
  void on_hblank();           // render the line, HBlank flag

  const u16* framebuffer(int screen) const { return fb_[screen]; }
  u16 line() const { return line_; }

private:
  NDS& nds_;
  u16 line_ = 0;
  u16 fb_[2][SCREEN_W * SCREEN_H] = {};
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2;   // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
