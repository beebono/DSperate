// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cstdlib>

namespace ds::gpu {

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }
static void ev_fifo(NDS& nds, u32 x)   { nds.gpu.on_display_fifo(x); }

Gpu::Gpu(NDS& nds) : engine{Engine2D(nds, 0), Engine2D(nds, 1)}, nds_(nds) {}

void Gpu::reset() {
  line_ = 0;
  frame_begun_ = false; screens_on_ = false; swap_ = false;
  master_bright_[0] = master_bright_[1] = 0;
  capcnt_ = 0; capture_on_ = false;
  fifo_.fill(0); fifo_rd_ = fifo_wr_ = 0; fifo_line_.fill(0); run_fifo_ = false;
  for (auto& fb : fb_) fb.fill(0);
  engine[0].reset(); engine[1].reset();
  set_powcnt(nds_.io.powcnt1);
  nds_.io.set_vcount(0);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

// ---- registers --------------------------------------------------------------

u32 Gpu::reg_read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  if (r >= 0x64 && r < 0x70) {
    auto rd16 = [&](u32 a) -> u32 {
      switch (a) {
      case 0x64: return capcnt_ & 0xFFFF;
      case 0x66: return capcnt_ >> 16;
      case 0x6C: return master_bright_[0];
      default: return 0;
      }
    };
    if (width == 32) return rd16(r) | (rd16(r + 2) << 16);
    if (width == 16) return rd16(r);
    return (rd16(r & ~1u) >> ((r & 1) * 8)) & 0xFF;
  }
  if (r >= 0x1064 && r < 0x1070) {
    const u32 v = (r & ~1u) == 0x106C ? master_bright_[1] : 0;
    return width == 8 ? (v >> ((r & 1) * 8)) & 0xFF : v;
  }
  return engine[r >= 0x1000].read(addr, width);
}

void Gpu::reg_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  if (r >= 0x64 && r < 0x70) {
    if (width == 32) {
      switch (r) {
      case 0x64: capcnt_ = value & 0xEF3F1F1F; return;
      case 0x68: fifo_[fifo_wr_] = value & 0xFFFF; fifo_[fifo_wr_ + 1] = value >> 16; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
      case 0x6C: master_bright_[0] = value & 0xC01F; return;
      default: return;
      }
    }
    if (width == 16) {
      switch (r) {
      case 0x64: capcnt_ = (capcnt_ & 0xFFFF0000) | (value & 0x1F1F); return;
      case 0x66: capcnt_ = (capcnt_ & 0x0000FFFF) | ((value & 0xEF3F) << 16); return;
      case 0x68: fifo_[fifo_wr_] = value; return;
      case 0x6A: fifo_[fifo_wr_ + 1] = value; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;   // the write pointer advances on the high half
      case 0x6C: master_bright_[0] = value & 0xC01F; return;
      default: return;
      }
    }
    switch (r) {
    case 0x64: capcnt_ = (capcnt_ & 0xFFFFFF00) | (value & 0x1F); return;
    case 0x65: capcnt_ = (capcnt_ & 0xFFFF00FF) | ((value & 0x1F) << 8); return;
    case 0x66: capcnt_ = (capcnt_ & 0xFF00FFFF) | ((value & 0x3F) << 16); return;
    case 0x67: capcnt_ = (capcnt_ & 0x00FFFFFF) | ((value & 0xEF) << 24); return;
    case 0x68: fifo_[fifo_wr_] = static_cast<u16>(value * 0x0101); return;
    case 0x6A: fifo_[fifo_wr_ + 1] = static_cast<u16>(value * 0x0101); return;
    case 0x6B: fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
    case 0x6C: master_bright_[0] = (master_bright_[0] & 0xFF00) | (value & 0x1F); return;
    case 0x6D: master_bright_[0] = (master_bright_[0] & 0x00FF) | ((value & 0xC0) << 8); return;
    default: return;
    }
  }
  if (r >= 0x1064 && r < 0x1070) {
    if (r == 0x106C && width >= 16) master_bright_[1] = value & 0xC01F;
    else if (r == 0x106C) master_bright_[1] = (master_bright_[1] & 0xFF00) | (value & 0x1F);
    else if (r == 0x106D) master_bright_[1] = (master_bright_[1] & 0x00FF) | ((value & 0xC0) << 8);
    return;
  }
  engine[r >= 0x1000].write(addr, width, value);
}

void Gpu::set_powcnt(u16 value) {
  engine[0].set_enabled(value & (1 << 1));
  engine[1].set_enabled(value & (1 << 9));
  nds_.gpu3d.set_powcnt(value);
  swap_ = value & (1 << 15);
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  engine[0].pre_draw(frame_reset);
  engine[1].pre_draw(frame_reset);
  if (line_ < 192) {
    draw_line(line_);
    // Sprites are rendered one line ahead of the backgrounds.
    if (line_ < 191) { DS_PROF(OBJ_DRAW); engine[0].render_sprites(line_ + 1); engine[1].render_sprites(line_ + 1); }
    nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  } else if (line_ == 215) {
    // The 3D frame flushed at VBlank is rasterised now, ahead of the next
    // frame's display lines.
    nds_.gpu3d.render_frame();
  } else if (line_ == 262) {
    engine[0].render_sprites(0); engine[1].render_sprites(0);
  }
  engine[0].post_draw(frame_reset);
  engine[1].post_draw(frame_reset);
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.event_time() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

void Gpu::on_scanline_start() {
  nds_.io.set_hblank(false);
  line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  engine[0].update_windows(line_);
  engine[1].update_windows(line_);
  if (line_ == 0) {
    begin_frame();
    nds_.frame_ready = true;
  } else if (line_ == 192) {
    nds_.io.set_vblank(true);
    fifo_rd_ = fifo_wr_ = 0;
    nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.dma.check(Cpu::ARM9, dma::MODE9_VBLANK);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_VBLANK);
    nds_.gpu3d.vblank();
    if (capture_on_) { capcnt_ &= ~(1u << 31); capture_on_ = false; }
  } else if (line_ == 262) nds_.io.set_vblank(false);
  if (line_ >= 2 && line_ < 194) nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  else if (line_ == 194) nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  if (line_ < 192 && run_fifo_) nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 32 * 2, ev_fifo, 0);
  nds_.io.set_vcount(line_);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.event_time() + HBLANK_START, ev_hblank);
}

void Gpu::begin_frame() {
  frame_begun_ = true;
  if (std::getenv("DS_DEBUG_GPU"))
    std::fprintf(stderr, "[gpu] frame %llu powcnt %04x dispcntA %08x dispcntB %08x mb %04x/%04x cap %08x vramcnt %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 static_cast<unsigned long long>(nds_.frame_count), nds_.io.powcnt1, engine[0].dispcnt(), engine[1].dispcnt(), master_bright_[0], master_bright_[1], capcnt_,
                 nds_.io.vramcnt[0], nds_.io.vramcnt[1], nds_.io.vramcnt[2], nds_.io.vramcnt[3], nds_.io.vramcnt[4], nds_.io.vramcnt[5], nds_.io.vramcnt[6], nds_.io.vramcnt[7], nds_.io.vramcnt[8]);
  if (std::getenv("DS_DEBUG_VRAMNZ")) {
    std::fprintf(stderr, "[vram] frame %llu nz:", static_cast<unsigned long long>(nds_.frame_count));
    for (int i = 0; i < 9; ++i) { u32 n = 0; const u8* b = nds_.bus.vram_bank(i); for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) n += b[k] != 0; std::fprintf(stderr, " %c=%u", 'A' + i, n); }
    for (int i : {5, 7}) { const u8* b = nds_.bus.vram_bank(i); u32 lo = ~0u, hi = 0; for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) if (b[k]) { if (k < lo) lo = k; hi = k; } std::fprintf(stderr, " %c[%x..%x]", 'A' + i, lo, hi); }
    std::fputc('\n', stderr);
  }
  if (const char* f = std::getenv("DS_DEBUG_DUMP_FRAME")) {   // with DS_DEBUG_DUMP_LINE=L: engine state and one rendered line
    if (nds_.frame_count == static_cast<u64>(std::atoi(f))) {
      const char* l = std::getenv("DS_DEBUG_DUMP_LINE"); const u32 line = l ? std::atoi(l) : 96;
      engine[0].debug_dump(line); engine[1].debug_dump(line);
    }
  }
  screens_on_ = nds_.io.powcnt1 & 1;
  // The FIFO only needs clocking when something displays or captures from it,
  // or a DMA channel is waiting on it.
  run_fifo_ = uses_fifo() || nds_.dma.in_mode(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
  if (capcnt_ & (1u << 31)) capture_on_ = true;
}

// ---- main-memory display FIFO -----------------------------------------------

bool Gpu::uses_fifo() const {
  if (((engine[0].dispcnt() >> 16) & 3) == 3) return true;
  return (capcnt_ & (1 << 25)) && ((capcnt_ >> 29) & 3) != 0;
}

void Gpu::sample_fifo(u32 offset, u32 count) {
  for (u32 i = 0; i < count; ++i) { fifo_line_[offset + i] = fifo_[fifo_rd_]; fifo_rd_ = (fifo_rd_ + 1) & 0xF; }
}

// The FIFO is read out in 8-pixel steps starting ~3 pixels before the visible
// line, so the sampling is offset from the 8-pixel DMA grid. Each step
// requests the next DMA transfer (start mode 4).
void Gpu::on_display_fifo(u32 x) {
  if (x > 0) { if (x == 8) sample_fifo(0, 5); else sample_fifo(x - 11, 8); }
  if (x < 256) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 6 * 8 * 2, ev_fifo, x + 8);
  } else sample_fifo(253, 3);
}

// ---- output stage -----------------------------------------------------------

void Gpu::draw_line(u32 line) {
  line3d_ = nds_.gpu3d.line(line);
  engine[0].set_3d_line(line3d_);
  engine[0].render_line(line);
  engine[1].render_line(line);
  DS_PROF(OUTPUT);
  u32* dst_a = fb_[swap_ ? 0 : 1].data() + line * SCREEN_W;
  u32* dst_b = fb_[swap_ ? 1 : 0].data() + line * SCREEN_W;
  if (screens_on_) {
    // The common display modes go through one fused kernel (copy, master
    // brightness, 6->8 bit expansion); the others build the line first.
    if (((engine[0].dispcnt() >> 16) & 3) == 1) kern::active::output_line(engine[0].output(), master_bright_[0], dst_a);
    else { output_a(line, dst_a); expand_colours(dst_a); }
    if ((engine[1].dispcnt() >> 16) & 1) kern::active::output_line(engine[1].output(), master_bright_[1], dst_b);
    else { output_b(line, dst_b); expand_colours(dst_b); }
  } else { for (u32 i = 0; i < 256; ++i) dst_a[i] = dst_b[i] = 0xFF000000; }
  if (capture_on_) { DS_PROF(CAPTURE); capture(line); }
}

static inline u32 rgb15_to_18_plain(u16 c) {
  return ((c & 0x001F) << 1) | (((c & 0x03E0) >> 4) << 8) | (((c & 0x7C00) >> 9) << 16);
}

void Gpu::output_a(u32 line, u32* dst) {
  const u32 dispcnt = engine[0].dispcnt();
  switch ((dispcnt >> 16) & 3) {
  case 0: for (u32 i = 0; i < 256; ++i) dst[i] = 0x3F3F3F; return;          // display off: white
  case 1: { const Pixel* src = engine[0].output(); for (u32 i = 0; i < 256; ++i) dst[i] = src[i]; break; }
  case 2: {                                                                 // VRAM display (LCDC bank)
    const u32 bank = (dispcnt >> 18) & 3;
    const VramMap& vm = nds_.bus.vram_map();
    if (vm.lcdc_mask & (1u << bank)) {
      const u16* src = reinterpret_cast<const u16*>(vm.bank(bank)) + line * 256;
      for (u32 i = 0; i < 256; ++i) dst[i] = rgb15_to_18_plain(src[i]);
    } else for (u32 i = 0; i < 256; ++i) dst[i] = 0;
    break;
  }
  case 3: for (u32 i = 0; i < 256; ++i) dst[i] = rgb15_to_18_plain(fifo_line_[i]); break;
  }
  apply_master_brightness(master_bright_[0], dst);
}

void Gpu::output_b(u32 line, u32* dst) {
  (void)line;
  if (!((engine[1].dispcnt() >> 16) & 1)) { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF3F3F3F; return; }
  const Pixel* src = engine[1].output();
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  apply_master_brightness(master_bright_[1], dst);
}

// Display capture: blends source A (engine A composite or the 3D layer) with
// source B (VRAM or the display FIFO) into an LCDC-mapped bank as BGR555.
void Gpu::capture(u32 line) {
  const u32 cnt = capcnt_;
  const u32 size = (cnt >> 20) & 3;
  const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
  if (line >= height) return;
  const u32 dst_bank = (cnt >> 16) & 3;
  const VramMap& vm = nds_.bus.vram_map();
  if (!(vm.lcdc_mask & (1u << dst_bank))) return;
  u16* dst = reinterpret_cast<u16*>(vm.bank(dst_bank)) + (((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF);

  const Pixel* src_a = (cnt & (1 << 24)) ? line3d_ : engine[0].output();
  const u16* src_b = nullptr;
  if (cnt & (1 << 25)) src_b = fifo_line_.data();
  else {
    const u32 dispcnt = engine[0].dispcnt();
    const u32 src_bank = (dispcnt >> 18) & 3;
    if (vm.lcdc_mask & (1u << src_bank)) {
      u32 off = line * 256;
      if (((dispcnt >> 16) & 3) != 2) off += ((cnt >> 26) & 3) << 14;
      src_b = reinterpret_cast<const u16*>(vm.bank(src_bank)) + (off & 0xFFFF);
    }
  }

  auto a15 = [&](u32 i, u32& r, u32& g, u32& b, u32& a) {
    const u32 v = src_a[i];
    r = (v >> 1) & 0x1F; g = (v >> 9) & 0x1F; b = (v >> 17) & 0x1F; a = (v >> 24) ? 1 : 0;
  };
  switch ((cnt >> 29) & 3) {
  case 0:
    for (u32 i = 0; i < width; ++i) { u32 r, g, b, a; a15(i, r, g, b, a); dst[i] = static_cast<u16>(r | (g << 5) | (b << 10) | (a << 15)); }
    break;
  case 1:
    if (src_b) for (u32 i = 0; i < width; ++i) dst[i] = src_b[i];
    else for (u32 i = 0; i < width; ++i) dst[i] = 0;
    break;
  default: {
    u32 eva = cnt & 0x1F, evb = (cnt >> 8) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    for (u32 i = 0; i < width; ++i) {
      u32 ra, ga, ba, aa; a15(i, ra, ga, ba, aa);
      u32 rb = 0, gb = 0, bb = 0, ab = 0;
      if (src_b) { const u16 v = src_b[i]; rb = v & 0x1F; gb = (v >> 5) & 0x1F; bb = (v >> 10) & 0x1F; ab = v >> 15; }
      u32 rd = ((ra * aa * eva) + (rb * ab * evb) + 8) >> 4;
      u32 gd = ((ga * aa * eva) + (gb * ab * evb) + 8) >> 4;
      u32 bd = ((ba * aa * eva) + (bb * ab * evb) + 8) >> 4;
      const u32 ad = (eva > 0 ? aa : 0) | (evb > 0 ? ab : 0);
      if (rd > 0x1F) rd = 0x1F;
      if (gd > 0x1F) gd = 0x1F;
      if (bd > 0x1F) bd = 0x1F;
      dst[i] = static_cast<u16>(rd | (gd << 5) | (bd << 10) | (ad << 15));
    }
    break;
  }
  }
}

void Gpu::apply_master_brightness(u16 reg, u32* dst) { kern::active::master_brightness(reg, dst); }

// 6-bit RGB666 records -> 8-bit 0xAARRGGBB (top two bits replicated into the low two).
void Gpu::expand_colours(u32* dst) { kern::active::expand_colours(dst); }

} // namespace ds::gpu
