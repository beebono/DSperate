// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ds::gpu {

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }
static void ev_fifo(NDS& nds, u32 x)   { nds.gpu.on_display_fifo(x); }

Gpu::Gpu(NDS& nds) : engine{Engine2D(nds, 0), Engine2D(nds, 1)}, nds_(nds) {
  // DS_2D_THREAD=0 keeps engine B on the emulation thread. The two paths must
  // produce identical frames; the env var is what makes that checkable.
  if (const char* l = std::getenv("DS_2D_LAZY")) lazy_enabled_ = std::atoi(l) != 0;
  if (const char* l = std::getenv("DS_2D_LAZY_CAPTURE")) lazy_capture_ = std::atoi(l) != 0;
  const char* e = std::getenv("DS_2D_THREAD");
  if (!e || std::atoi(e) != 0) {
    // The only statics the two engines share are the colour tables, built by
    // magic statics (thread-safe init) and read-only afterwards; every other
    // buffer, cache and palette copy is per-engine.
    eng_b_.start(&Gpu::engine_b_job, this);
    par_2d_ = eng_b_.running();
  }
  if (const char* l = std::getenv("DS_2D_LAG")) lag_enabled_ = std::atoi(l) != 0;
  if (const char* l = std::getenv("DS_2D_SPLIT")) split_ = std::atoi(l) != 0;   // opt-in, see gpu.h
}

void Gpu::reset() {
  join_b();
  disarm_trap();
  line_ = 0; hblank_done_ = false;
  lazy_frame_ = false; per_line_[0] = per_line_[1] = false;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = false;
  frame_begun_ = false; screens_on_ = false;
  master_bright_g_[0] = master_bright_g_[1] = 0;
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
      case 0x6C: return master_bright_g_[0];
      default: return 0;
      }
    };
    if (width == 32) return rd16(r) | (rd16(r + 2) << 16);
    if (width == 16) return rd16(r);
    return (rd16(r & ~1u) >> ((r & 1) * 8)) & 0xFF;
  }
  if (r >= 0x1064 && r < 0x1070) {
    const u32 v = (r & ~1u) == 0x106C ? master_bright_g_[1] : 0;
    return width == 8 ? (v >> ((r & 1) * 8)) & 0xFF : v;
  }
  return engine[r >= 0x1000].read(addr, width);
}

void Gpu::reg_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // MASTER_BRIGHT: guest copy here, render-side copy through the engine's journal.
  auto mb = [&](int e, u16 v) { master_bright_g_[e] = v; engine[e].master_bright_write(v); };
  if (r >= 0x64 && r < 0x70) {
    if (width == 32) {
      switch (r) {
      case 0x64: capcnt_ = value & 0xEF3F1F1F; return;
      case 0x68: fifo_[fifo_wr_] = value & 0xFFFF; fifo_[fifo_wr_ + 1] = value >> 16; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    if (width == 16) {
      switch (r) {
      case 0x64: capcnt_ = (capcnt_ & 0xFFFF0000) | (value & 0x1F1F); return;
      case 0x66: capcnt_ = (capcnt_ & 0x0000FFFF) | ((value & 0xEF3F) << 16); return;
      case 0x68: fifo_[fifo_wr_] = value; return;
      case 0x6A: fifo_[fifo_wr_ + 1] = value; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;   // the write pointer advances on the high half
      case 0x6C: mb(0, value & 0xC01F); return;
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
    case 0x6C: mb(0, (master_bright_g_[0] & 0xFF00) | (value & 0x1F)); return;
    case 0x6D: mb(0, (master_bright_g_[0] & 0x00FF) | ((value & 0xC0) << 8)); return;
    default: return;
    }
  }
  if (r >= 0x1064 && r < 0x1070) {
    if (r == 0x106C && width >= 16) mb(1, value & 0xC01F);
    else if (r == 0x106C) mb(1, (master_bright_g_[1] & 0xFF00) | (value & 0x1F));
    else if (r == 0x106D) mb(1, (master_bright_g_[1] & 0x00FF) | ((value & 0xC0) << 8));
    return;
  }
  const int e = r >= 0x1000;
  engine[e].write(addr, width, value);
  // Engine A switching to VRAM display mid-frame starts reading an LCDC bank
  // the trap does not cover: render the rest of the frame per line.
  if (e == 0 && (r & 0xFFF) < 4 && trap_armed_ && !trap_lcdc_ && ((engine[0].read(0x04000000, 32) >> 16) & 3) == 2) fall_back_per_line(3);
}

void Gpu::set_powcnt(u16 value) {
  engine[0].powcnt_write(value);
  engine[1].powcnt_write(value);
  nds_.gpu3d.set_powcnt(value);
}

// ---- slow-path stores and the VRAM trap ---------------------------------------

// Palette and OAM: the guest bytes change now, the engine copy through the
// journal. A store of the value already there changes nothing anywhere and is
// not journaled (silent-store elimination). Byte stores are applied, as the
// direct mapping applied them before.
void Gpu::palette_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.palette.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].palette_written(off & 0x3FF, width, value);
}
void Gpu::oam_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.oam.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].oam_written(off & 0x3FF, width, value);
}

// A store into VRAM the engines can see, before it lands. Only ARM9 reaches
// the engine windows; the LCDC window matters only while it is displayed.
void Gpu::vram_store_trap(Cpu cpu, u32 addr) {
  if (!trap_armed_ || cpu != Cpu::ARM9) return;
  if (addr >= 0x06800000 && !trap_lcdc_) return;
  const u32 mask = store_engines(addr);
  // Already per-line everywhere this store can reach: nothing to do but the
  // lag-mode join below.
  if ((per_line_[0] || !(mask & 1)) && (per_line_[1] || !(mask & 2))) {
    // Lag mode: the store may land on a line engine B is still drawing.
    prof::add(prof::C_2D_LAG_STORES, 1);
    if (b_inflight_) prof::add(prof::C_2D_LAG_STORE_JOINS, 1);
    join_b();
    if (++lag_trap_hits_ >= LAG_TRAP_LIMIT) { lag_frame_ = false; disarm_trap(); prof::add(prof::C_2D_LAG_DROPPED, 1); }
    return;
  }
  prof::add(prof::C_2D_TRAP_HITS, 1);
  // Burst only the engines this store can actually reach. The budget is per
  // engine: one engine streaming tiles must not spend the other's.
  u32 burst_mask = 0;
  for (int e = 0; e < 2; ++e)
    if ((mask & (1u << e)) && !per_line_[e] && ++lazy_bursts_[e] < LAZY_BURST_LIMIT) burst_mask |= 1u << e;
  if (burst_mask) {
    // Lines whose HBlank has passed are drawn before the bytes change; the
    // burst continues per line, and the frame re-batches when it ends.
    catch_up(burst_mask);
    for (int e = 0; e < 2; ++e)
      if (burst_mask & (1u << e)) { per_line_[e] = true; burst_[e] = true; burst_left_[e] = LAZY_BURST_LINES; }
    // The trap stays armed while an engine is still batching: it is that
    // engine's guard, and the bursting one re-arms when its window ends.
    if (per_line_[0] && per_line_[1]) disarm_trap();
    return;
  }
  fall_back_per_line(mask);
}

bool Gpu::vram_remap_begin() {
  catch_up(3);
  join_b();
  const bool was = trap_armed_;
  if (was) disarm_trap();
  return was;
}
void Gpu::vram_remap_end(bool trapped) { if (trapped) arm_trap(); }

void Gpu::arm_trap() {
  // LCDC banks are trapped when engine A displays one -- or when a capture
  // writes one, since the batched capture reads its source B and writes its
  // destination there in line order.
  trap_lcdc_ = ((engine[0].dispcnt() >> 16) & 3) == 2 || capture_on_;
  nds_.bus.set_vram_trap(true, trap_lcdc_);
  trap_armed_ = true;
}
void Gpu::disarm_trap() {
  if (!trap_armed_) return;
  nds_.bus.set_vram_trap(false, trap_lcdc_);
  trap_armed_ = false;
}

// The DS maps BG and OBJ VRAM to fixed address ranges, one engine each, so a
// store's address alone says which engine can see it. LCDC (0x06800000+) is
// charged to both: it is a bank alias, and engine A can display it directly.
u32 Gpu::store_engines(u32 addr) const {
  if (!split_) return 3;
  if ((addr >> 24) != 0x06) return 3;
  switch ((addr >> 21) & 3) {   // 0x000000 bgA, 0x200000 bgB, 0x400000 objA, 0x600000 objB
  case 0: case 2: return 1;
  case 1: case 3: return 2;
  }
  return 3;
}

void Gpu::catch_up(u32 mask) {
  const u32 f = frontier();
  if (f == 0) return;
  const u32 last = (f < SCREEN_H ? f : SCREEN_H) - 1;
  const bool a = (mask & 1) && render_next_[0] <= last && render_next_[0] < SCREEN_H;
  const bool b = (mask & 2) && render_next_[1] <= last && render_next_[1] < SCREEN_H;
  if (!a && !b) return;
  render_ranges(a ? render_next_[0] : 1, a ? last : 0,
                b ? render_next_[1] : 1, b ? last : 0);
}
void Gpu::fall_back_per_line(u32 mask) {
  catch_up(mask);
  for (int e = 0; e < 2; ++e) if (mask & (1u << e)) { per_line_[e] = true; burst_[e] = false; }
  // The trap now guards the line in flight instead of the batch -- but only
  // once no engine is still batching, or the other engine loses its guard.
  if (!(lag_frame_ && par_2d_) && per_line_[0] && per_line_[1]) disarm_trap();
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  if (line_ < SCREEN_H) {
    // Display line: rendered now in per-line mode, or all together at the
    // last one. The per-line latches (pre/post_draw, the sprites one line
    // ahead) run inside step_engine, in front of the journal replay.
    // Each engine is either batching (render everything at the last line) or
    // per-line. With DS_2D_SPLIT off the two are always in the same mode and
    // this is the old single range.
    u32 f[2], l[2];
    for (int e = 0; e < 2; ++e) {
      const bool batch = lazy_frame_ && !per_line_[e];
      if (batch && line_ != SCREEN_H - 1) { f[e] = 1; l[e] = 0; continue; }   // nothing yet
      f[e] = batch ? render_next_[e] : line_;
      l[e] = batch ? SCREEN_H - 1 : line_;
      if (!batch && render_next_[e] < line_) f[e] = render_next_[e];          // catch up anything skipped
    }
    render_ranges(f[0], l[0], f[1], l[1]);
    // End of a burst window: the lines after this one batch again, trapped.
    for (int e = 0; e < 2; ++e)
      if (burst_[e] && --burst_left_[e] == 0 && line_ < SCREEN_H - 1) {
        burst_[e] = false; per_line_[e] = false; render_next_[e] = line_ + 1; arm_trap();
      }
    hblank_done_ = true;
    nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  } else {
    hblank_done_ = true;
    join_b();
    engine[0].pre_draw(line_, frame_reset);
    engine[1].pre_draw(line_, frame_reset);
    if (line_ == 215) {
      // The 3D frame flushed at VBlank is rasterised now, ahead of the next
      // frame's display lines.
      nds_.gpu3d.render_frame();
      if (probe_enabled_) async_probe_start();
    } else if (line_ == 262) {
      engine[0].render_sprites(0); engine[1].render_sprites(0);
    }
    engine[0].post_draw(frame_reset);
    engine[1].post_draw(frame_reset);
  }
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.event_time() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

// ---- async-raster probe (DS_ASYNC_PROBE=1, measurement only) ----------------
//
// An asynchronous 3D raster would start at line 215 and have to be joined
// before its output is read. Two candidate deadlines: line 0 of the next
// frame (join everything before the first display line) and the next swap at
// line 192 (the loosest possible, with a per-band lazy join). The probe
// hashes the texture and texture-palette VRAM at the start and at each
// deadline: a change means a worker would have been reading bytes the CPU was
// writing. VRAMCNT rewrites are counted separately in Bus::update_vram --
// those rebuild the view arrays themselves, which is worse than torn texels.
namespace {
u64 hash_view(const VramView& v) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 b = 0; b < v.blocks(); ++b) {
    const u8* p = v.ptr[b];
    if (!p) { h = (h ^ 0x9e37) * 0x100000001b3ull; continue; }
    for (u32 o = 0; o < VramView::BLOCK; o += 8) {
      u64 w; std::memcpy(&w, p + o, 8);
      h = (h ^ w) * 0x100000001b3ull;
    }
  }
  return h;
}
u64 hash_tex_vram(const VramMap& vm) { return hash_view(vm.texture) * 31 + hash_view(vm.texpal); }
} // namespace

void Gpu::async_probe_start() {
  const VramMap& vm = nds_.bus.vram_map();
  probe_hash_ = hash_tex_vram(vm);
  probe_open_ = true;
  probe_vramcnt_at_l0_ = 0;
  prof::async_window = true;
  prof::add(prof::C_ASYNC_FRAMES, 1);
}

void Gpu::async_probe_check(bool at_line0) {
  if (!probe_open_) return;
  const bool dirty = hash_tex_vram(nds_.bus.vram_map()) != probe_hash_;
  if (at_line0) {
    if (dirty) prof::add(prof::C_ASYNC_DIRTY_L0, 1);
    // Snapshot the remap count so far; the rest belongs to the wider window.
    probe_vramcnt_at_l0_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
    if (probe_vramcnt_at_l0_ != probe_vramcnt_base_) prof::add(prof::C_ASYNC_VRAMCNT_L0, 1);
    return;
  }
  if (dirty) prof::add(prof::C_ASYNC_DIRTY_SWAP, 1);
  probe_open_ = false;
  prof::async_window = false;
  probe_vramcnt_base_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
}

void Gpu::on_scanline_start() {
  nds_.io.set_hblank(false);
  line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  hblank_done_ = false;

  // Display lines evaluate their window edges inside step_engine; the rest
  // of the frame is applied directly (nothing is pending by then).
  if (line_ >= SCREEN_H) { join_b(); engine[0].update_windows(line_); engine[1].update_windows(line_); }
  if (line_ == 0) {
    if (probe_enabled_) async_probe_check(true);
    begin_frame();
    nds_.frame_ready = true;
  } else if (line_ == 192) {
    if (probe_enabled_) async_probe_check(false);
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
                 static_cast<unsigned long long>(nds_.frame_count), nds_.io.powcnt1, engine[0].dispcnt(), engine[1].dispcnt(), master_bright_g_[0], master_bright_g_[1], capcnt_,
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
  // The frame's rendering mode. The FIFO is sampled per line and capture
  // writes VRAM the guest may read back per line: both stay per-line.
  render_next_[0] = render_next_[1] = 0;
  per_line_[0] = per_line_[1] = false; frame_finished_ = false;
  lazy_frame_ = lazy_enabled_ && !run_fifo_ && (!capture_on_ || lazy_capture_);
  lazy_bursts_[0] = lazy_bursts_[1] = 0; burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0;
  // Lag mode for the per-line lines of this frame: the trap guards the line
  // in flight (capture writes only LCDC banks, which no engine reads, so
  // capture itself never needs a join).
  lag_frame_ = lag_enabled_ && par_2d_;
  lag_trap_hits_ = 0;
  if (lazy_frame_ || lag_frame_) arm_trap();
  if (lazy_frame_) prof::add(prof::C_2D_LAZY_FRAMES, 1);
  if (lag_frame_ && !lazy_frame_) prof::add(prof::C_2D_LAG_FRAMES, 1);
  if (!lazy_frame_) per_line_[0] = per_line_[1] = true;
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

// ---- rendering and the output stage ------------------------------------------

// Engine B's share of a run of display lines. Nothing here is reachable from
// engine A: the engines' state is disjoint and the output stage writes each
// engine's screen and line buffer.
void Gpu::engine_b_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  for (u32 l = g.eng_b_first_; l <= g.eng_b_last_; ++l) g.step_engine(1, l);
}

void Gpu::debug_dump(FILE* f) {
  std::fprintf(f, "  gpu: line %u hblank_done %d render_next %u/%u lazy %d per_line %d/%d trap %d eng_b %u..%u par_2d %d frame_ready %d\n", line_, hblank_done_ ? 1 : 0, render_next_[0], render_next_[1],
               lazy_frame_ ? 1 : 0, per_line_[0] ? 1 : 0, per_line_[1] ? 1 : 0, trap_armed_ ? 1 : 0, eng_b_first_, eng_b_last_, par_2d_ ? 1 : 0, nds_.frame_ready ? 1 : 0);
  eng_b_.debug_dump(f);
  nds_.gpu3d.debug_dump(f);
}

// Render display lines [first, last] of both engines: one hand-off to the
// worker for engine B's run, engine A's run here.
// Per-engine ranges. Engine B's goes to the worker first so it overlaps engine
// A's here; an empty range (first > last) means that engine has nothing due.
void Gpu::render_ranges(u32 af, u32 al, u32 bf, u32 bl) {
  const bool a_has = af <= al, b_has = bf <= bl;
  if (!a_has && !b_has) return;
  // A short run against a parked worker is drawn here: the wake-up costs more
  // than engine B's lines do, and the trap-hit catch-ups of a batched frame
  // (Golden Sun: ~16 a frame, 8-line bursts between them) would each pay it --
  // measured as the whole loss of batching on that title. The frame's main
  // batch, or any run the worker is already hot for, is handed off as before.
  bool b_handed = false;
  if (b_has && par_2d_ && (bl - bf + 1 >= 24 || !eng_b_.parked())) {
    join_b();                               // the previous run, if it was left in flight
    eng_b_first_ = bf; eng_b_last_ = bl;
    eng_b_.dispatch();
    b_inflight_ = true;
    b_handed = true;
  }
  if (a_has) prof::add(prof::C_2D_RANGE_A, 1);
  if (b_has) prof::add(prof::C_2D_RANGE_B, 1);
  if (a_has) for (u32 x = af; x <= al; ++x) step_engine(0, x);
  if (b_has && !b_handed) for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
  // A per-line run may stay in flight until the next line under DS_2D_LAG;
  // the last display line always joins, since writes after it apply directly.
  if (b_handed && lag_frame_ && per_line_[1] && bl < SCREEN_H - 1) prof::add(prof::C_2D_LAG_LINES, 1);
  else join_b();
  if (a_has) render_next_[0] = al + 1;
  if (b_has) render_next_[1] = bl + 1;
  if (!frame_finished_ && render_next_[0] >= SCREEN_H && render_next_[1] >= SCREEN_H) {
    frame_finished_ = true;
    join_b();
    engine[0].frame_done(); engine[1].frame_done();
    disarm_trap();
  }
}

// One engine's display line, in hardware order: the writes stamped before
// the scanline start, the window edges, the writes of the line itself, the
// HBlank latches, the line, its output, the next line's sprites.
void Gpu::step_engine(int e, u32 line) {
  Engine2D& en = engine[e];
  en.replay_to(line * 2);
  en.update_windows(line);
  en.replay_to(line * 2 + 1);
  en.pre_draw(line, false);
  if (e == 0) { line3d_ = nds_.gpu3d.line(line); en.set_3d_line(line3d_); }
  en.render_line(line);
  output_engine(e, line);
  if (e == 0 && capture_on_) { DS_PROF(CAPTURE); capture(line); }
  // Sprites are rendered one line ahead of the backgrounds.
  if (line < SCREEN_H - 1) {
    prof::Scope* sc = (e == 0 && prof::enabled) ? new prof::Scope(prof::OBJ_DRAW) : nullptr;
    en.render_sprites(line + 1);
    delete sc;
  }
  en.post_draw(false);
}

// The output stage for one engine's line: display mode, master brightness,
// 6->8 bit expansion, into the screen POWCNT1 bit 15 gives it (or scaled
// straight into the frontend's buffer).
void Gpu::output_engine(int e, u32 line) {
  prof::Scope* sc = (e == 0 && prof::enabled) ? new prof::Scope(prof::OUTPUT) : nullptr;
  const Engine2D& en = engine[e];
  const int screen = en.screen();
  const bool scaled = scaling();
  u32* dst = scaled ? line_out_[e].data() : fb_[screen].data() + line * SCREEN_W;
  if (screens_on_) {
    // The common display modes go through one fused kernel (copy, master
    // brightness, 6->8 bit expansion); the others build the line first.
    if (e == 0) {
      if (((en.dispcnt() >> 16) & 3) == 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else { output_a(line, dst); expand_colours(dst); }
    } else {
      if ((en.dispcnt() >> 16) & 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else { output_b(dst); expand_colours(dst); }
    }
  } else { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF000000; }
  if (scaled) emit_scaled(screen, line, dst);
  delete sc;
}

// One source line to the destination rows it covers. Destination row y takes
// source row y * 192 / h, so source line `line` owns rows
// [ceil(line*h/192), ceil((line+1)*h/192)) -- usually two or three of them.
// The first is scaled and the rest copied from it: at that point it is the
// hottest line in the machine, and a copy beats redoing the run fill.
// Mean of four 0xAARRGGBB pixels, per channel, alpha forced opaque.
static inline u32 mean4(u32 a, u32 b, u32 c, u32 d) {
  const u32 rb = (((a & 0xFF00FFu) + (b & 0xFF00FFu) + (c & 0xFF00FFu) + (d & 0xFF00FFu) + 0x020002u) >> 2) & 0xFF00FFu;
  const u32 g  = (((a & 0xFF00u) + (b & 0xFF00u) + (c & 0xFF00u) + (d & 0xFF00u) + 0x0200u) >> 2) & 0xFF00u;
  return 0xFF000000u | rb | g;
}
// The colour at least two of the four share (ties to the earlier pixel), or their mean when all differ.
static inline u32 mode4(u32 a, u32 b, u32 c, u32 d) {
  if (a == b || a == c || a == d) return a;
  if (b == c || b == d) return b;
  if (c == d) return c;
  return mean4(a, b, c, d);
}

// Rec.601-ish luma, 0..255*256.
static inline u32 luma(u32 c) { return ((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29; }
static inline u32 min4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l < bl) { best = p; bl = l; } }
  return best;
}
static inline u32 max4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l > bl) { best = p; bl = l; } }
  return best;
}
// The darkest or brightest of the four, whichever stands farther from the
// block's mean luma, when that is by more than `thr` -- keeps a thin stroke
// whatever its polarity while dithers and gradients (small deviations)
// average; the mean otherwise, and when both are equally far.
static inline u32 extreme4(u32 a, u32 b, u32 c, u32 d, u32 thr) {
  const u32 lo = min4(a, b, c, d), hi = max4(a, b, c, d);
  const u32 m = (luma(a) + luma(b) + luma(c) + luma(d)) / 4;
  const u32 dlo = m - luma(lo), dhi = luma(hi) - m;
  if (dlo <= thr && dhi <= thr) return mean4(a, b, c, d);
  return dlo > dhi ? lo : dhi > dlo ? hi : mean4(a, b, c, d);
}

// sRGB <-> linear for the linear-light blend: 8-bit sRGB to 12-bit linear
// and back, built once.
namespace {
struct GammaLut {
  u16 to_lin[256];
  u8 from_lin[4096];
  GammaLut() {
    for (u32 i = 0; i < 256; ++i) {
      const double c = i / 255.0;
      const double l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
      to_lin[i] = static_cast<u16>(std::lround(l * 4095.0));
    }
    for (u32 i = 0; i < 4096; ++i) {
      const double l = i / 4095.0;
      const double c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
      from_lin[i] = static_cast<u8>(std::lround(c * 255.0));
    }
  }
};
const GammaLut& gamma_lut() { static const GammaLut lut; return lut; }

// Per-pixel weighted blend in linear light; w per pixel as blend_line_w.
void blend_line_linear(const u32* a, const u32* b, const u8* w, u32* out) {
  const GammaLut& g = gamma_lut();
  for (u32 i = 0; i < 256; ++i) {
    const u32 f = w[i];
    if (!f) { out[i] = a[i]; continue; }
    u32 r = 0xFF000000u;
    for (u32 sh = 0; sh < 24; sh += 8) {
      const u32 la = g.to_lin[(a[i] >> sh) & 255], lb = g.to_lin[(b[i] >> sh) & 255];
      r |= static_cast<u32>(g.from_lin[(la * (256 - f) + lb * f + 128) >> 8]) << sh;
    }
    out[i] = r;
  }
}
} // namespace

void Gpu::blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out) {
  alignas(16) u8 wt[SCREEN_W];
  std::memset(wt, static_cast<int>(w), sizeof wt);
  if (t.blend == 2) blend_line_linear(a, b, wt, out); else kern::active::blend_line_w(a, b, wt, out);
}

// One row with box-filter seams: the last pixel of a run that straddles two
// source pixels is their area-weighted blend (the previous block in chunky
// mode is never straddled: its boundaries are integer).
void Gpu::emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst) {
  alignas(16) u32 next[SCREEN_W], seam[SCREEN_W];
  std::memcpy(next, src + 1, (SCREEN_W - 1) * sizeof(u32));
  next[SCREEN_W - 1] = src[SCREEN_W - 1];
  if (t.blend == 2) blend_line_linear(src, next, t.seam_w, seam); else kern::active::blend_line_w(src, next, t.seam_w, seam);
  kern::active::scale_row_straddle(src, seam, t.seam_w, t.xrun, dst);
}

bool Gpu::build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a) {
  a.cells = cells; a.cell_px = cell_px;
  a.first.assign(cells, 0); a.n.assign(cells, 0); a.w.assign(static_cast<size_t>(cells) * CELL_TAPS, 0);
  for (u32 i = 0; i < cells; ++i) {
    // Cell i covers source [i*src_n/cells, (i+1)*src_n/cells); tap weights
    // are the overlap over the cell's width, in 1/256, the last one fixed
    // so that they sum to 256 exactly.
    const u32 lo = i * src_n, hi = (i + 1) * src_n;         // in 1/cells units
    const u32 s0 = lo / cells, s1 = (hi + cells - 1) / cells; // taps [s0, s1)
    if (s1 - s0 > CELL_TAPS) return false;
    a.first[i] = static_cast<u16>(s0); a.n[i] = static_cast<u8>(s1 - s0);
    u32 sum = 0;
    for (u32 s = s0; s < s1; ++s) {
      const u32 olo = std::max(lo, s * cells), ohi = std::min(hi, (s + 1) * cells);
      u32 w = ((ohi - olo) * 256 + src_n / 2) / src_n;
      if (s + 1 == s1) w = 256 - sum;
      sum += w;
      a.w[static_cast<size_t>(i) * CELL_TAPS + (s - s0)] = static_cast<u16>(w);
    }
  }
  return true;
}

// One cell row: every cell's colour from its taps in the held lines, then
// the row of cells drawn as `cell_px` panel rows (the first the seam row
// when the grid is on) through the grid kernel with the cells' xrun.
void Gpu::emit_cells(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const CellMap& m = *t.cells;
  const u32 j = cell_row_[screen];
  if (j >= m.y.cells) return;
  const u32 l0 = m.y.first[j], ln = m.y.n[j];
  std::memcpy(cell_lines_[screen][line % CELL_TAPS], src, SCREEN_W * sizeof(u32));
  if (line + 1 < l0 + ln) return;
  auto held = [&](u32 v) { return cell_lines_[screen][(l0 + v) % CELL_TAPS]; };
  // The row is complete.
  alignas(16) u32 cells[SCREEN_W];
  const bool linear = t.blend == 2;
  const GammaLut& g = gamma_lut();
  const u16* wy = &m.y.w[static_cast<size_t>(j) * CELL_TAPS];
  for (u32 i = 0; i < m.x.cells; ++i) {
    const u32 s0 = m.x.first[i], sn = m.x.n[i];
    const u16* wx = &m.x.w[static_cast<size_t>(i) * CELL_TAPS];
    // Mean: the 2D box (weights in 1/65536), in sRGB or linear light.
    u32 acc[3] = {0, 0, 0};
    for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
      const u32 c = held(v)[s0 + u], wt = wx[u] * wy[v];
      for (u32 ch = 0; ch < 3; ++ch) { const u32 b = (c >> (8 * ch)) & 255; acc[ch] += (linear ? g.to_lin[b] : b) * wt; }
    }
    u32 mean = 0xFF000000u;
    for (u32 ch = 0; ch < 3; ++ch) {
      const u32 v = (acc[ch] + 32768) >> 16;
      mean |= static_cast<u32>(linear ? g.from_lin[std::min<u32>(v, 4095)] : v) << (8 * ch);
    }
    u32 out = mean;
    if (t.chunky != 2) {
      // The other modes work on the pixels that are mostly inside the cell
      // (at least half covered on both axes); the largest-coverage pixel
      // stands in for "top-left".
      u32 cand[CELL_TAPS * CELL_TAPS]; u32 nc = 0;
      u32 best = 0, bestw = 0;
      for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
        const u32 wt = wx[u] * wy[v];
        if (wt > bestw) { bestw = wt; best = held(v)[s0 + u]; }
        if (wx[u] >= 128 && wy[v] >= 128) cand[nc++] = held(v)[s0 + u];
      }
      if (nc == 0) cand[nc++] = best;
      switch (t.chunky) {
      case 1: out = best; break;
      case 3: {   // dominant: the most repeated candidate, ties to the earlier; the mean when all differ
        u32 bc = 0; out = mean;
        for (u32 a = 0; a < nc; ++a) { u32 cnt = 0; for (u32 b = 0; b < nc; ++b) cnt += cand[b] == cand[a]; if (cnt > bc && cnt >= 2) { bc = cnt; out = cand[a]; } }
        break;
      }
      case 4: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) < luma(out)) out = cand[a]; break; }
      case 5: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) > luma(out)) out = cand[a]; break; }
      default: {  // extreme: the darkest or brightest candidate, if farther than the threshold from the mean
        u32 lo = cand[0], hi = cand[0];
        for (u32 a = 1; a < nc; ++a) { if (luma(cand[a]) < luma(lo)) lo = cand[a]; if (luma(cand[a]) > luma(hi)) hi = cand[a]; }
        const u32 lm = luma(mean), dlo = lm > luma(lo) ? lm - luma(lo) : 0, dhi = luma(hi) > lm ? luma(hi) - lm : 0;
        out = (dlo <= t.chunky_thresh && dhi <= t.chunky_thresh) ? mean : dlo > dhi ? lo : dhi > dlo ? hi : mean;
      }
      }
    }
    cells[i] = out;
  }
  for (u32 i = m.x.cells; i < SCREEN_W; ++i) cells[i] = 0;
  // Draw: rows [j*P, (j+1)*P), the first the seam row.
  const u32 P = m.y.cell_px;
  const u32 y0 = j * P;
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  const bool grid = t.grid < 256;
  u32* row = t.px + static_cast<size_t>(y0 + (grid ? 1 : 0)) * t.pitch;
  if (grid) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, false, row);
  else      kern::active::scale_row(cells, t.xrun, row);
  for (u32 y = y0 + (grid ? 2 : 1); y < y0 + P; ++y)
    std::memcpy(t.px + static_cast<size_t>(y) * t.pitch, row, bytes);
  if (grid) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, true, t.px + static_cast<size_t>(y0) * t.pitch);
  cell_row_[screen] = j + 1;
}

void Gpu::emit_scaled(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  if (t.chunky && t.cells) {
    if (line == 0) cell_row_[screen] = 0;
    emit_cells(screen, line, src);
    return;
  }
  u32 first = line, last = line;
  alignas(16) u32 block[SCREEN_W];
  if (t.chunky) {
    // Each 2x2 block of DS pixels is one cell (the frontend's xrun merges
    // the pixel pairs; the line pair is merged here). Top-left draws from
    // the even line as it arrives; mean and mode hold the even line and
    // resolve the block when the odd one completes it. Only the even
    // pixels' entries are read (the odd runs are empty).
    if (t.chunky == 1) { if (line & 1) return; last = line + 1; }
    else {
      if (!(line & 1)) { std::memcpy(chunk_even_[screen], src, sizeof block); return; }
      first = line - 1;
      const u32* up = chunk_even_[screen];
      switch (t.chunky) {
      case 2:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mean4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 3:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mode4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 4:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = min4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 5:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = max4(up[s], up[s + 1], src[s], src[s + 1]); break;
      default: for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = extreme4(up[s], up[s + 1], src[s], src[s + 1], t.chunky_thresh); break;
      }
      src = block;
    }
  }
  const u32 y0 = (first * t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 y1 = ((last + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (y0 >= y1) return;                      // downscale: this line is dropped
  u32* row = t.px + static_cast<size_t>(y0) * t.pitch;
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  if (t.blend && t.seam_w) {
    // Box-filter seams (sharp-shimmerless): a panel pixel or row that
    // straddles two source pixels or lines is their area-weighted blend,
    // every other one is nearest. The straddling row of this span is its
    // last, and needs the next line, so it is written when that arrives;
    // the crisp rows go out now.
    const u32 hb = (last + 1) * t.h;                 // this span's lower boundary, in 1/192 rows
    const bool straddle_below = (hb % SCREEN_H) != 0 && last + 1 < SCREEN_H;
    const u32 ycrisp_end = straddle_below ? y1 - 1 : y1;
    if (ycrisp_end > y0) {
      emit_row_straddle(t, src, row);
      for (u32 y = y0 + 1; y < ycrisp_end; ++y)
        std::memcpy(t.px + static_cast<size_t>(y) * t.pitch, row, bytes);
    }
    // The row above this span straddles the previous line and this one.
    if (first > 0 && seam_prev_line_[screen] + 1 == first) {
      const u32 tb = first * t.h;                    // this span's upper boundary
      const u32 frac = tb % SCREEN_H;
      if (frac) {
        alignas(16) u32 mid[SCREEN_W];
        blend_rows(t, seam_prev_[screen], src, (frac * 256) / SCREEN_H, mid);   // weight of this line
        emit_row_straddle(t, mid, t.px + static_cast<size_t>(y0 - 1) * t.pitch);
      }
    }
    std::memcpy(seam_prev_[screen], src, sizeof seam_prev_[screen]);
    seam_prev_line_[screen] = last;
    return;
  }
  if (t.grid >= 256) {
    kern::active::scale_row(src, t.xrun, row);
    for (u32 y = y0 + 1; y < y1; ++y)
      std::memcpy(t.px + static_cast<size_t>(y) * t.pitch, row, bytes);
    return;
  }
  // LCD grid: the last output column of every source pixel's run and the last
  // output row of every source line's span are dimmed, so each DS pixel shows
  // as a lit cell with a dark seam right and below. Runs are whatever the
  // fractional scale hands out (2 and 3 wide at 2.5x), so the seams are not
  // evenly spaced there -- a one-pixel seam per DS pixel is what a panel
  // that size can honestly show. A run of one pixel is left alone: dimming
  // it would erase the pixel, not outline it. The seam row is scaled from
  // the source too, so the corner where the two seams meet is dimmed once.
  //
  // Only runs the fractional part of the scale widened carry a seam
  // (min_run = ceil(scale)): the lit cell keeps the integer size everywhere,
  // and at 2.5x the seams fall on every other DS pixel rather than every
  // other DS pixel being half-width. The seam leads its run (see
  // scale_row_grid), so the seam row is the span's first row and the plain
  // row is built below it. At an integer scale every run qualifies. Rows
  // follow the same rule as columns.
  const u32 w = t.xrun[SCREEN_W];
  const u32 min_run = (w + SCREEN_W - 1) / SCREEN_W, min_rows = (t.h + SCREEN_H - 1) / SCREEN_H;
  const bool seam = y1 - y0 >= std::max<u32>(2, min_rows);
  const u32 yfirst = seam ? y0 + 1 : y0;
  row = t.px + static_cast<size_t>(yfirst) * t.pitch;
  kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, false, row);
  for (u32 y = yfirst + 1; y < y1; ++y)
    std::memcpy(t.px + static_cast<size_t>(y) * t.pitch, row, bytes);
  if (seam) kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, true, t.px + static_cast<size_t>(y0) * t.pitch);
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
  apply_master_brightness(engine[0].master_bright(), dst);
}

void Gpu::output_b(u32* dst) {
  if (!((engine[1].dispcnt() >> 16) & 1)) { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF3F3F3F; return; }
  const Pixel* src = engine[1].output();
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  apply_master_brightness(engine[1].master_bright(), dst);
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


void Gpu::quiesce() {
  join_b();
  engine[0].apply_pending(); engine[1].apply_pending();
}

void Gpu::prepare_load() {
  join_b();
  disarm_trap();
  lazy_frame_ = false; per_line_[0] = per_line_[1] = true;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = true;
  burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0; lag_frame_ = false; b_inflight_ = false;
}

template <class S> void Gpu::sync_state(S& s) {
  s.begin("GPU ");
  s.fields(line_, hblank_done_, frame_begun_, screens_on_, master_bright_g_, capcnt_, capture_on_, fifo_, fifo_rd_, fifo_wr_, fifo_line_, run_fifo_);
  s.fields(fb_);   // what the display shows until the next frame (and the thumbnail)
  s.end();
  engine[0].sync_state(s);
  engine[1].sync_state(s);
  if constexpr (S::reading) {
    nds_.sched.rebind(EventId::HBlank, ev_hblank);
    nds_.sched.rebind(EventId::VBlank_Scanline, ev_scanline);
    nds_.sched.rebind(EventId::DisplayFifo, ev_fifo);
  }
}
template void Gpu::sync_state<state::Writer>(state::Writer&);
template void Gpu::sync_state<state::Reader>(state::Reader&);

void Gpu::after_load() {
  // The state was taken right after line 0's begin_frame(): its decisions
  // depend only on registers and DMA state, all restored, so retaking them
  // reproduces the frame's mode and re-arms the trap.
  if (frame_begun_ && line_ == 0 && !hblank_done_) begin_frame();
}

} // namespace ds::gpu
