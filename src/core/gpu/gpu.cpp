// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
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
  if (const char* l = std::getenv("DS_2D_LAG")) lag_enabled_ = std::atoi(l) != 0;   // opt-in, see gpu.h
}

void Gpu::reset() {
  join_b();
  disarm_trap();
  line_ = 0; hblank_done_ = false;
  lazy_frame_ = per_line_ = false; render_next_ = SCREEN_H;
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
  if (e == 0 && (r & 0xFFF) < 4 && trap_armed_ && !trap_lcdc_ && ((engine[0].read(0x04000000, 32) >> 16) & 3) == 2) fall_back_per_line();
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
  if (per_line_) {
    // Lag mode: the store may land on a line engine B is still drawing.
    prof::add(prof::C_2D_LAG_STORES, 1);
    if (b_inflight_) prof::add(prof::C_2D_LAG_STORE_JOINS, 1);
    join_b();
    if (++lag_trap_hits_ >= LAG_TRAP_LIMIT) { lag_frame_ = false; disarm_trap(); prof::add(prof::C_2D_LAG_DROPPED, 1); }
    return;
  }
  prof::add(prof::C_2D_TRAP_HITS, 1);
  if (lazy_frame_ && !per_line_ && ++lazy_bursts_ < LAZY_BURST_LIMIT) {
    // Lines whose HBlank has passed are drawn before the bytes change; the
    // burst continues per line, and the frame re-batches when it ends.
    catch_up();
    disarm_trap();
    per_line_ = true; burst_ = true; burst_left_ = LAZY_BURST_LINES;
    return;
  }
  fall_back_per_line();
}

bool Gpu::vram_remap_begin() {
  catch_up();
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

void Gpu::catch_up() {
  const u32 f = frontier();
  if (render_next_ < f && render_next_ < SCREEN_H) render_lines(render_next_, (f < SCREEN_H ? f : SCREEN_H) - 1);
}
void Gpu::fall_back_per_line() {
  catch_up();
  per_line_ = true; burst_ = false;
  // The trap now guards the line in flight instead of the batch.
  if (!(lag_frame_ && par_2d_)) disarm_trap();
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  if (line_ < SCREEN_H) {
    // Display line: rendered now in per-line mode, or all together at the
    // last one. The per-line latches (pre/post_draw, the sprites one line
    // ahead) run inside step_engine, in front of the journal replay.
    if (lazy_frame_ && !per_line_) { if (line_ == SCREEN_H - 1) render_lines(render_next_, SCREEN_H - 1); }
    else {
      render_lines(line_, line_);
      // End of a burst window: the lines after this one batch again, trapped.
      if (burst_ && --burst_left_ == 0 && line_ < SCREEN_H - 1) { burst_ = false; per_line_ = false; arm_trap(); }
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
  render_next_ = 0;
  per_line_ = false;
  lazy_frame_ = lazy_enabled_ && !run_fifo_ && (!capture_on_ || lazy_capture_);
  lazy_bursts_ = 0; burst_ = false; burst_left_ = 0;
  // Lag mode for the per-line lines of this frame: the trap guards the line
  // in flight (capture writes only LCDC banks, which no engine reads, so
  // capture itself never needs a join).
  lag_frame_ = lag_enabled_ && par_2d_;
  lag_trap_hits_ = 0;
  if (lazy_frame_ || lag_frame_) arm_trap();
  if (lazy_frame_) prof::add(prof::C_2D_LAZY_FRAMES, 1);
  if (lag_frame_ && !lazy_frame_) prof::add(prof::C_2D_LAG_FRAMES, 1);
  if (!lazy_frame_) per_line_ = true;
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
  std::fprintf(f, "  gpu: line %u hblank_done %d render_next %u lazy %d per_line %d trap %d eng_b %u..%u par_2d %d frame_ready %d\n", line_, hblank_done_ ? 1 : 0, render_next_,
               lazy_frame_ ? 1 : 0, per_line_ ? 1 : 0, trap_armed_ ? 1 : 0, eng_b_first_, eng_b_last_, par_2d_ ? 1 : 0, nds_.frame_ready ? 1 : 0);
  eng_b_.debug_dump(f);
  nds_.gpu3d.debug_dump(f);
}

// Render display lines [first, last] of both engines: one hand-off to the
// worker for engine B's run, engine A's run here.
void Gpu::render_lines(u32 first, u32 last) {
  // A short run against a parked worker is drawn here: the wake-up costs
  // more than engine B's lines do, and the trap-hit catch-ups of a batched
  // frame (Golden Sun: ~14 a frame, 8-line bursts between them) would each
  // pay it -- measured as the whole loss of batching on that title. The
  // frame's main batch, or any run the worker is already hot for, is handed
  // off as before.
  if (par_2d_ && (last - first + 1 >= 24 || !eng_b_.parked())) {
    join_b();                               // the previous run, if it was left in flight
    eng_b_first_ = first; eng_b_last_ = last;
    eng_b_.dispatch();
    for (u32 l = first; l <= last; ++l) step_engine(0, l);
    // A per-line run stays in flight until the next line (or a join point);
    // the last display line joins now, since writes after it apply directly.
    if (lag_frame_ && per_line_ && last < SCREEN_H - 1) { b_inflight_ = true; prof::add(prof::C_2D_LAG_LINES, 1); }
    else eng_b_.wait();
  } else {
    for (u32 l = first; l <= last; ++l) step_engine(0, l);
    for (u32 l = first; l <= last; ++l) step_engine(1, l);
  }
  render_next_ = last + 1;
  if (render_next_ == SCREEN_H) {
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
void Gpu::emit_scaled(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const u32 y0 = (line * t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 y1 = ((line + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (y0 >= y1) return;                      // downscale: this line is dropped
  u32* row = t.px + static_cast<size_t>(y0) * t.pitch;
  kern::active::scale_row(src, t.xrun, row);
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  for (u32 y = y0 + 1; y < y1; ++y)
    std::memcpy(t.px + static_cast<size_t>(y) * t.pitch, row, bytes);
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
  lazy_frame_ = false; per_line_ = true; render_next_ = SCREEN_H;
  burst_ = false; burst_left_ = 0; lag_frame_ = false; b_inflight_ = false;
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
