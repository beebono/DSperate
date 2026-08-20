// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
namespace ds { struct NDS; }
namespace ds::io {
// IRQ controller, timers, DMA, IPC FIFO/sync, RTC/SPI, cart bus. Each gets its
// own file as it is implemented; this is the register dispatch.
class Io {
public:
  explicit Io(NDS& nds) : nds_(nds) {}
  void reset() {}
  u32  read (Cpu cpu, u32 addr, u32 width) { return 0; }
  void write(Cpu cpu, u32 addr, u32 width, u32 value) {}
private:
  NDS& nds_;
};
}
