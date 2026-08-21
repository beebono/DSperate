// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::dma {

// Start modes, normalised so ARM7 modes don't collide with ARM9 ones.
enum Mode : u32 {
  MODE9_IMMEDIATE = 0, MODE9_VBLANK = 1, MODE9_HBLANK = 2, MODE9_DISPLAY_START = 3,
  MODE9_DISPLAY_FIFO = 4, MODE9_CART = 5, MODE9_GBA = 6, MODE9_GXFIFO = 7,
  MODE7_IMMEDIATE = 0x10, MODE7_VBLANK = 0x11, MODE7_CART = 0x12, MODE7_WIFI_GBA = 0x13,
};

struct Channel {
  Cpu cpu; int num;
  u32 src = 0, dst = 0, cnt = 0;
  u32 cur_src = 0, cur_dst = 0;
  s32 src_inc = 1, dst_inc = 1;
  u32 start_mode = 0;
  u32 rem_count = 0, iter_count = 0;
  u32 running = 0;          // 0 idle, 1 running, 2 running (first access of a burst)
  bool in_progress = false;
  const u8* burst_table = nullptr; u32 burst_pos = 0;
};

// Eight DMA channels (4 per CPU). A running channel stalls its CPU; the
// scheduler runs `run()` in the CPU's place until the transfer (or its current
// iteration) completes. Unit timings follow the main-RAM burst model of the
// hardware as documented by melonDS.
class Dma {
public:
  explicit Dma(NDS& nds);
  void reset();

  void write_src(Cpu cpu, int ch, u32 v);
  void write_dst(Cpu cpu, int ch, u32 v);
  void write_cnt(Cpu cpu, int ch, u32 v);
  u32  read_cnt(Cpu cpu, int ch) const { return channel(cpu, ch).cnt; }
  u32  read_src(Cpu cpu, int ch) const { return channel(cpu, ch).src; }
  u32  read_dst(Cpu cpu, int ch) const { return channel(cpu, ch).dst; }

  // Trigger / cancel channels waiting on a start condition.
  void check(Cpu cpu, u32 mode);
  void stop(Cpu cpu, u32 mode);
  bool any_running(Cpu cpu) const;
  bool in_mode(Cpu cpu, u32 mode) const;

  // Run the CPU's DMA channels for up to `budget` cycles (that CPU's clock).
  // Returns cycles consumed.
  u32 run(Cpu cpu, u32 budget);

private:
  NDS& nds_;
  std::array<Channel, 8> ch_;
  Channel& channel(Cpu cpu, int n) { return ch_[static_cast<int>(cpu) * 4 + n]; }
  const Channel& channel(Cpu cpu, int n) const { return ch_[static_cast<int>(cpu) * 4 + n]; }
  void start(Channel& c);
  u32  run_channel(Channel& c, u32 budget);
  u32  unit_cycles(Channel& c, bool burst_start, bool word);
};

} // namespace ds::dma
