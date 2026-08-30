// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/line_worker.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

// Display timing, the two 2D engines and the output stage (display modes,
// master brightness, display capture, main-memory FIFO). The 3D pipeline
// plugs in through Engine2D::set_3d_line().
//
// Lazy 2D. The engines are not rendered at each HBlank. Writes that can
// change the picture are journaled per engine with the display line they
// first affect (journal_stamp), and the frame is rendered in one batch at the
// last display line's HBlank -- engine B on the line worker, engine A here,
// one hand-off a frame -- replaying the journal in front of each line, so a
// scroll register written every HBlank still lands on the right line. What
// the journal cannot cover is VRAM: stores into the pages the engines read
// are trapped (Bus::set_vram_trap) for the display period, and the first one
// in a frame renders every line whose HBlank has already passed *before* the
// bytes change, renders the next eight lines per line (no trap needed while
// every line is drawn at its own HBlank), then re-arms and batches again;
// a frame with many such bursts stays per line. VRAMCNT remaps catch up the
// same way and stay lazy. Capture frames batch as well: capture runs inside
// the batch in line order and its LCDC banks are trapped, so the captured
// bytes land at the last display line rather than per line -- the one
// accepted departure from hardware, visible only to a CPU read of the
// capture bank before then (DS_2D_LAZY_CAPTURE=0 restores per line). Frames
// that display or capture from the display FIFO run per line: that input is
// per line by nature. DS_2D_LAZY=0 forces per-line rendering (through the
// same journal), which must produce identical frames.
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
  void on_hblank();           // render (lazily), HBlank flag
  void on_display_fifo(u32 x);
  void begin_frame();
  void async_probe_start();
  void async_probe_check(bool at_line0);
  bool probe_enabled_ = std::getenv("DS_ASYNC_PROBE") != nullptr;
  bool probe_open_ = false;
  u64  probe_hash_ = 0, probe_vramcnt_at_l0_ = 0, probe_vramcnt_base_ = 0;         // latches POWCNT/FIFO/capture state; runs at line 0 (and once before the first frame)
  bool frame_begun() const { return frame_begun_; }
  bool at_line_start() const { return !hblank_done_; }

  // Save states, taken at the start of line 0 (where run_frame() returns).
  // quiesce() joins the engine-B thread and drains the journals without
  // changing what the guest sees; prepare_load() also lifts the VRAM trap
  // so the old frame's lazy state cannot fire on the new memory; after_load()
  // re-takes the frame's lazy/trap decision exactly as begin_frame() did.
  void quiesce();
  void prepare_load();
  template <class S> void sync_state(S& s);
  void after_load();

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

  // Journal stamp for a write happening now: twice the first display line it
  // can affect, plus one when that line's scanline start has already passed
  // (the window edges are evaluated there, before the line's own writes).
  // NO_STAMP when the write lands after the last display line of the frame
  // is rendered -- then nothing is pending and it applies at once.
  static constexpr u32 NO_STAMP = 0xFFFF;
  u32 journal_stamp() const {
    const u32 l = hblank_done_ ? line_ + 1u : line_;
    return l < SCREEN_H ? l * 2 + (hblank_done_ ? 0 : 1) : NO_STAMP;
  }
  // Slow-path stores (Bus): palette / OAM land in the guest bytes and the
  // engine's journal; a VRAM store on a trapped page catches the render up.
  void palette_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void oam_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void vram_store_trap(Cpu cpu, u32 addr);
  bool vram_remap_begin();            // before a VRAMCNT remap: catch up, lift the trap; returns whether it was set
  void vram_remap_end(bool trapped);  // after: re-arm it
  void set_lazy(bool on) { lazy_enabled_ = on; }   // tests: DS_2D_LAZY

  const u32* framebuffer(int screen) const { return fb_[screen].data(); }   // 0 = top, 1 = bottom

  // A frontend-owned, panel-sized destination for one screen. When set, the
  // output stage scales each line into it as the line is produced instead of
  // filling fb_ for the frontend to rescale afterwards: the source line is
  // still in L1 at that moment, which is most of the point -- rescaling the
  // finished framebuffer re-reads it cold, and that read is what makes the
  // separate pass slow. fb_ is left untouched while a target is set; nothing
  // else reads it (display capture works off the engine's own output).
  struct ScaleTarget {
    u32* px = nullptr;          // top-left of this screen's rect in the frontend's buffer
    u32 pitch = 0;              // destination pitch, in u32
    u32 h = 0;                  // destination rect height, in pixels
    const u16* xrun = nullptr;  // 257 entries; see kern::scale_row
  };
  // Both screens or neither: pass a null `px` to go back to fb_.
  void set_scale_target(int screen, const ScaleTarget& t) { scale_[screen] = t; }
  bool scaling() const { return scale_[0].px && scale_[1].px; }
  u16 line() const { return line_; }

  Engine2D engine[2];

private:
  NDS& nds_;
  u16 line_ = 0;
  bool hblank_done_ = false;  // this line's HBlank event has run (its render, if any, is behind us)
  bool frame_begun_ = false;
  bool screens_on_ = false;   // POWCNT1 bit 0, latched at frame start
  u16 master_bright_g_[2] = {0, 0};   // guest-visible; the engines hold the render-side value
  u32 capcnt_ = 0;
  bool capture_on_ = false;
  std::array<u16, 16> fifo_{};
  u8 fifo_rd_ = 0, fifo_wr_ = 0;
  alignas(16) std::array<u16, 256> fifo_line_{};
  bool run_fifo_ = false;
  std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb_{};
  ScaleTarget scale_[2];
  // The output stage's line buffer when scaling: output_line writes here
  // instead of into fb_, at the same cost, and scale_row reads it back hot.
  alignas(16) std::array<std::array<u32, SCREEN_W>, 2> line_out_{};
  const u32* line3d_ = nullptr;   // 3D output for the line being drawn (engine A's thread)

  // Lazy-2D state for the frame in progress.
  bool lazy_enabled_ = true;      // DS_2D_LAZY != 0
  bool lazy_frame_ = false;       // this frame may batch
  bool per_line_ = false;         // ... but has fallen back to per-line rendering
  u32  render_next_ = SCREEN_H;   // display lines rendered so far this frame
  bool trap_armed_ = false, trap_lcdc_ = false;
  // Capture frames batch too (DS_2D_LAZY_CAPTURE=0 keeps them per line; see
  // the class comment). A trapped store catches the frame up, lifts the trap and renders the
  // next LAZY_BURST_LINES lines per line (exact without any trap, and no
  // slow-path stores while a DMA streams), then re-arms and batches again.
  // Two page-table walks per burst rather than two per line, and no trapped
  // store inside it. Past LAZY_BURST_LIMIT bursts the frame stays per line.
  bool lazy_capture_ = true;
  bool burst_ = false;            // per-line for the current burst of stores
  u32  burst_left_ = 0;           // display lines left before re-batching
  u32  lazy_bursts_ = 0;
  static constexpr u32 LAZY_BURST_LIMIT = 16, LAZY_BURST_LINES = 8;
  u32  frontier() const { return hblank_done_ ? line_ + 1u : line_; }   // first line a write now can still affect
  void catch_up();                // render every line below the frontier
  void fall_back_per_line();      // catch up and render the rest of the frame per line
  void arm_trap();
  void disarm_trap();
  void render_lines(u32 first, u32 last);   // both engines, [first, last]
  void step_engine(int e, u32 line);        // one engine's display line: replay, latches, render, output

  // Engine B's lines run on the worker while engine A's run here. The two
  // engines share no mutable state -- the only statics they reach are the
  // read-only colour tables -- and no CPU runs inside the callback, so the
  // pair sees exactly the register and VRAM state the sequential order
  // saw. DS_2D_THREAD=0 forces the sequential path for comparison.
  LineWorker eng_b_;
public:
  // DS_WATCHDOG: where the display pipeline stands when a frame stalls.
  void debug_dump(FILE* f);
private:
  u32 eng_b_first_ = 0, eng_b_last_ = 0;
  bool par_2d_ = false;
  // Per-line frames (capture, the display FIFO, a VRAM trap) hand engine B
  // one line at a time. Rather than wait for it at once -- which needs the
  // worker hot, i.e. spinning through the whole frame on a core the raster
  // workers want -- the line is left in flight and joined a line later
  // (b_inflight_), so the worker can park between lines with no cost to the
  // emulation thread. What must join earlier: the last display line (writes
  // after it apply directly), a VRAMCNT remap, and a guest store into VRAM
  // the engines read (the trap stays armed in these frames for that; past
  // LAG_TRAP_LIMIT hits in a frame the lag is dropped and the trap lifted,
  // so a game streaming VRAM per line pays neither).
  // Off by default: on GSDD the lag never pays -- its capture frames stream
  // ~3,500 VRAM stores a frame, so the armed trap costs more than the core it
  // frees (RG DS, 2026-08-29: +3.4 % with the lag, +1.3 % lag with the old
  // spin budget, +1.7 % parking alone; replay scenes flat). DS_2D_LAG=1.
  bool lag_enabled_ = false;      // DS_2D_LAG=1
  bool b_inflight_ = false;
  bool lag_frame_ = false;        // this frame's per-line lines may stay in flight
  u32  lag_trap_hits_ = 0;
  static constexpr u32 LAG_TRAP_LIMIT = 4096;   // GSDD traps ~55 stores a frame in capture frames; 64 dropped the lag every frame
  void join_b() { if (b_inflight_) { eng_b_.wait(); b_inflight_ = false; } }
public:
  void journal_full() { join_b(); }   // Engine2D::queue on a full journal
private:
  static void engine_b_job(void* self);

  void output_engine(int e, u32 line);
  void emit_scaled(int screen, u32 line, const u32* src);
  void output_a(u32 line, u32* dst);
  void output_b(u32* dst);
  void capture(u32 line);
  void apply_master_brightness(u16 reg, u32* dst);
  void expand_colours(u32* dst);
  bool uses_fifo() const;
  void sample_fifo(u32 offset, u32 count);
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2;   // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
