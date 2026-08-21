// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/engine2d.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

// Display timing, the two 2D engines and the output stage (display modes,
// master brightness, display capture, main-memory FIFO). The 3D pipeline
// plugs in through Engine2D::set_3d_line() later.
//
// Framebuffers are 256x192 u32 per screen in 0xAARRGGBB with 8-bit channels
// expanded from the hardware's 6 bits, the same layout melonDS produces, so
// frames can be compared byte for byte against the reference.
class Gpu {
public:
  explicit Gpu(NDS& nds);
  void reset();

  // Scheduler callbacks.
  void on_scanline_start();   // VCOUNT advance, VBlank/VCount flags
  void on_hblank();           // render the line, HBlank flag
  void on_display_fifo(u32 x);
  void begin_frame();         // latches POWCNT/FIFO/capture state; runs at line 0 (and once before the first frame)
  bool frame_begun() const { return frame_begun_; }

  // 2D register file (0x04000000-0x0400006F, 0x04001000-0x0400106F) minus
  // DISPSTAT/VCOUNT, which stay with the interrupt logic in Io.
  static bool owns_reg(u32 addr) {
    const u32 r = addr - 0x04000000;
    if (r < 0x70) return (r >= 8 || r < 4) && (r < 0x60 || r >= 0x64);   // 0x60 is DISP3DCNT
    return r >= 0x1000 && r < 0x1070 && (r < 0x1004 || r >= 0x1008);
  }
  u32  reg_read(u32 addr, u32 width);
  void reg_write(u32 addr, u32 width, u32 value);
  void set_powcnt(u16 value);

  const u32* framebuffer(int screen) const { return fb_[screen].data(); }   // 0 = top, 1 = bottom
  u16 line() const { return line_; }

  Engine2D engine[2];

private:
  NDS& nds_;
  u16 line_ = 0;
  bool frame_begun_ = false;
  bool screens_on_ = false;   // POWCNT1 bit 0, latched at frame start
  bool swap_ = false;         // POWCNT1 bit 15: engine A on the top screen
  u16 master_bright_[2] = {0, 0};
  u32 capcnt_ = 0;
  bool capture_on_ = false;
  std::array<u16, 16> fifo_{};
  u8 fifo_rd_ = 0, fifo_wr_ = 0;
  alignas(16) std::array<u16, 256> fifo_line_{};
  bool run_fifo_ = false;
  std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb_{};
  const u32* line3d_ = nullptr;   // 3D output for the line being drawn

  void draw_line(u32 line);
  void output_a(u32 line, u32* dst);
  void output_b(u32 line, u32* dst);
  void capture(u32 line);
  void apply_master_brightness(u16 reg, u32* dst);
  void expand_colours(u32* dst);
  bool uses_fifo() const;
  void sample_fifo(u32 offset, u32 count);
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2;   // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
