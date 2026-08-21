// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include "core/nds.h"

namespace ds::gpu {

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }

void Gpu::reset() {
  line_ = 0;
  nds_.io.set_vcount(0);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

void Gpu::on_hblank() {
  nds_.io.set_hblank(true);
  if (line_ < 192) nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  // TODO: render line_ for both engines when line_ < 192.
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.now() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

void Gpu::on_scanline_start() {
  nds_.io.set_hblank(false);
  line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  if (line_ == 192) {
    nds_.io.set_vblank(true);
    nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.dma.check(Cpu::ARM9, dma::MODE9_VBLANK);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_VBLANK);
  } else if (line_ == 262) nds_.io.set_vblank(false);
  if (line_ >= 2 && line_ < 194) nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  if (line_ == 0) nds_.frame_ready = true;
  nds_.io.set_vcount(line_);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

} // namespace ds::gpu
