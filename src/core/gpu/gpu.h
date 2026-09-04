#include <vector>
// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/line_worker.h"
#include "core/gpu/render3d.h"
#include "core/profile.h"

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
// last display line's HBlank -- engine A on the worker thread, joined only
// at the start of line 0 (see worker_), engine B here, one hand-off a frame
// -- replaying the journal in front of each line, so a
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

  // Journal stamp for a write to engine `e` happening now: twice the first
  // display line it can affect, plus one when that line's scanline start has
  // already passed (the window edges are evaluated there, before the line's
  // own writes). NO_STAMP when the write lands after the last display line
  // of the frame is rendered and nothing of that engine is in flight -- then
  // it applies at once. While the engine's lines are still being drawn on
  // the worker (engine A's batch, until line 0) the VBlank lines stamp too,
  // and the entries are applied in order at the join.
  static constexpr u32 NO_STAMP = 0xFFFF;
  u32 journal_stamp(int e) const {
    const u32 l = hblank_done_ ? line_ + 1u : line_;
    return (l < SCREEN_H || inflight_[e]) ? l * 2 + (hblank_done_ ? 0 : 1) : NO_STAMP;
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

  // A screen the frontend does not show (single-screen layouts). The engine
  // driving it -- through POWCNT1's swap bit, checked per line -- skips its
  // drawing (backgrounds, sprites, output) while its journal, latches,
  // windows and lazy-2D bookkeeping keep running, so it is exact again the
  // line it is shown. Engine A never skips: display capture reads its
  // output. The hidden screen's framebuffer is stale meanwhile. Set between
  // frames only (the worker reads it during one).
  void set_screen_visible(int screen, bool on) { screen_visible_[screen] = on; }

  // Frameskip (frontend policy; see the SDL frontend's [emu] frameskip). A
  // skipped frame runs the machine unchanged -- the CPUs, the journals, the
  // latches, the geometry -- and only leaves out what nothing else observes:
  // both engines' line rendering and output, and the 3D rasterisation that
  // feeds them. The framebuffers keep the last drawn frame, so the frontend
  // simply does not present.
  //
  // The 3D raster for a frame runs at line 215 of the frame before it, so the
  // decision has to be one frame ahead of the display lines it governs: the
  // flag set here is taken at line 215 and applies to the *next* frame's
  // display, which is what will_skip_frame() reports back. A frame that
  // display-captures or feeds the display FIFO is never skipped -- both write
  // bytes the guest reads back -- which is settled per frame for the 2D and
  // predicted from the current frame at line 215 for the 3D.
  void set_frame_skip(bool on) { skip_req_ = on; }
  // Skip frames that display-capture as well (INEXACT, [emu] frameskip_capture).
  // The capture write is skipped along with the drawing, so the destination
  // bank keeps the picture it last captured; DISPCAPCNT itself behaves exactly
  // as before. A game that reads the captured pixels back with the CPU, rather
  // than only displaying them, sees an older frame than the hardware would.
  void set_frameskip_capture(bool on) { skip_capture_ok_ = on; }

  // How many frames it takes the display setup to come back round. Games drive
  // the two screens on alternate frames: Golden Sun swaps POWCNT1's screen bit
  // every frame and renders one screen's content each time, and a capture can
  // alternate between two destination banks the same way. Drawing one frame in
  // a multiple of this period would then draw the same phase for ever -- one
  // screen frozen on its off-frame, which reads as the two screens swapping --
  // so a frontend must keep its drawn cadence off a multiple of it (1 = no
  // alternation, nothing to avoid). Watched: the POWCNT1 swap bit, each
  // engine's display mode and VRAM display bank, and the capture destination.
  u8 display_phase_period() const { return phase_period_; }
  bool lines_in_flight() const { return inflight_[0] || inflight_[1]; }
  // Bus::vram_read on an LCDC page under the capture read trap: join the
  // lines in flight if the read is of the bank the capture is writing.
  bool lcdc_read_trapped() const { return read_trap_bank_ >= 0; }
  void lcdc_read_hit(u32 addr) {
    if (static_cast<int>((addr >> 17) & 7) == read_trap_bank_) { prof::add(prof::C_2D_A_JOIN_READS, 1); join_worker(); }
  }
  // begin_frame() for the frame about to run has already happened when
  // run_frame() returns, so this is settled before the frontend asks.
  bool will_skip_frame() const { return skip_frame_; }

  // A frontend-owned, panel-sized destination for one screen. When set, the
  // output stage scales each line into it as the line is produced instead of
  // filling fb_ for the frontend to rescale afterwards: the source line is
  // still in L1 at that moment, which is most of the point -- rescaling the
  // finished framebuffer re-reads it cold, and that read is what makes the
  // separate pass slow. fb_ is left untouched while a target is set; nothing
  // else reads it (display capture works off the engine's own output).
  // Chunky with a cell of P panel pixels: cells_x * cells_y cells, each an
  // area-weighted box of the DS pixels it covers (k = 256 / cells_x of them
  // per axis, not necessarily an integer). Per axis, cell i takes taps
  // first[i] .. first[i] + n[i] - 1 with weights w[i][..] summing to 256.
  static constexpr u32 CELL_TAPS = 8;
  struct CellAxis {
    u32 cells = 0, cell_px = 0;      // count, and panel pixels per cell
    std::vector<u16> first;          // per cell
    std::vector<u8>  n;
    std::vector<u16> w;              // cells * CELL_TAPS
  };
  struct CellMap { CellAxis x, y; };
  // Fills `a` for `cells` cells over `src_n` source pixels, `cell_px` panel
  // pixels each. False if a cell would need more than CELL_TAPS taps.
  static bool build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a);

  struct ScaleTarget {
    u32* px = nullptr;          // top-left of this screen's rect in the frontend's buffer
    u32 pitch = 0;              // destination pitch, in u32
    u32 h = 0;                  // destination rect height, in pixels
    const u16* xrun = nullptr;  // 257 entries; see kern::scale_row
    u32 grid = 256;             // LCD grid: brightness kept on the grid lines, 0..256 (256 = no grid)
    u8 chunky = 0;              // 0 off; else each 2x2 block of DS pixels is one cell (xrun merges pixel pairs):
                                // 1 top-left pixel, 2 mean of the four, 3 dominant colour (mean when all differ),
                                // 4 darkest, 5 brightest, 6 the one whose luma is farthest from the mean, if by more
                                // than chunky_thresh (else the mean)
    u32 chunky_thresh = 180 * 256;  // luma units (0..255 * 256); mode 6 only
    u8 blend = 0;               // box-filter seams (sharp-shimmerless): 1 blend in sRGB, 2 in linear light
    const u8* seam_w = nullptr; // 256 entries: weight (0..255 = 0..1) of pixel s+1 in run s's last pixel; 0 = no straddle
    const CellMap* cells = nullptr; // chunky with a panel-sized cell (see CellMap); null = the 2x2 pair path
  };
  // Both screens or neither: pass a null `px` to go back to fb_.
  void set_scale_target(int screen, const ScaleTarget& t) { scale_[screen] = t; }
  // Run a whole DS-resolution image through the scanline scaler, for a
  // picture the emulator did not produce: the frontend's pause menu, which is
  // composited while nothing is running and so has no display lines of its
  // own. The grid, chunky and seam treatment are the ones the game gets, so
  // the menu sits in the same picture rather than beside it. No-op when this
  // screen has no scale target (the renderer tier, or a hidden screen).
  void scale_image(int screen, const u32* src);
  bool scaling() const { return scale_[0].px && scale_[1].px; }
  u16 line() const { return line_; }

  Engine2D engine[2];

  // Both screens forced to plain white by MASTER_BRIGHT: mode 1 (brightness
  // up) at full factor on each engine, whatever they are drawing underneath.
  // That is how the firmware's own fades end, and reading the register beats
  // scanning two framebuffers for it -- the pixels are only white a frame
  // later, and on the fast scaling path they are never in fb_ to scan at all.
  bool screens_forced_white() const {
    for (u16 mb : master_bright_g_)
      if ((mb >> 14) != 1 || (mb & 0x1F) < 16) return false;
    return true;
  }

private:
  NDS& nds_;
  u16 line_ = 0;
  bool hblank_done_ = false;  // this line's HBlank event has run (its render, if any, is behind us)
  bool frame_begun_ = false;
  bool screens_on_ = false;   // POWCNT1 bit 0, latched at frame start
  bool screen_visible_[2] = {true, true};
  bool skipped_[2] = {false, false};   // this engine's last line was skipped: its next drawn line re-renders its sprites
  bool skip_req_ = false;     // frameskip: the frontend's request, taken at line 215
  bool skip_next_ = false;    // taken there: the next frame's display lines are skipped
  bool skip_frame_ = false;   // latched in begin_frame from skip_next_, gated by skippable()
  // Never skip a frame the guest reads back: one that display-captures or
  // feeds the display FIFO. capture_recent_ keeps that true for a few frames
  // after the last capture as well, because the 3D raster is skipped a frame
  // ahead of the display it feeds -- a game that captures every other frame
  // would otherwise capture a stale 3D picture into VRAM.
  static constexpr u8 CAPTURE_STICKY = 8;
  u8 capture_recent_ = 0;
  bool skip_capture_ok_ = false;
  bool skippable() const { return !run_fifo_ && (skip_capture_ok_ || (!capture_on_ && !capture_recent_)); }
  // Display-phase detection (see display_phase_period). The signature of the
  // last PHASE_HISTORY frames, newest last, and the smallest period that
  // explains them.
  static constexpr u32 PHASE_HISTORY = 8, PHASE_MAX = 4;
  u32 phase_sig_[PHASE_HISTORY] = {};
  u32 phase_seen_ = 0;
  u8 phase_period_ = 1;
  void update_phase();
  u16 master_bright_g_[2] = {0, 0};   // guest-visible; the engines hold the render-side value
  u32 capcnt_ = 0;
  bool capture_on_ = false;
  std::array<u16, 16> fifo_{};
  u8 fifo_rd_ = 0, fifo_wr_ = 0;
  alignas(16) std::array<u16, 256> fifo_line_{};
  bool run_fifo_ = false;
  std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb_{};
  ScaleTarget scale_[2];
  alignas(16) u32 chunk_even_[2][SCREEN_W];   // chunky: the even line, held until the odd one completes the block
  alignas(16) u32 seam_prev_[2][SCREEN_W];    // blend: the previous source row, for the straddling row
  u32 seam_prev_line_[2] = {~0u, ~0u};
  // Cell chunky: the last CELL_TAPS source lines, a ring by line number
  // (adjacent cell rows share a line, so a row's taps are read from it).
  alignas(16) u32 cell_lines_[2][CELL_TAPS][SCREEN_W];
  u32 cell_row_[2] = {0, 0};        // the cell row being gathered
  void emit_cells(int screen, u32 line, const u32* src);
  void emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst);
  void blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out);
  // The output stage's line buffer when scaling: output_line writes here
  // instead of into fb_, at the same cost, and scale_row reads it back hot.
  alignas(16) std::array<std::array<u32, SCREEN_W>, 2> line_out_{};
  const u32* line3d_ = nullptr;   // 3D output for the line being drawn (whichever thread draws engine A)

  // Lazy-2D state for the frame in progress.
  bool lazy_enabled_ = true;      // DS_2D_LAZY != 0
  bool lazy_frame_ = false;       // this frame may batch
  // Per engine. A store into one engine's BG/OBJ VRAM cannot change what the
  // other engine fetches, so only that engine has to leave the batch: Golden
  // Sun streams ~96 KB a frame into engine B's BG and took engine A -- the
  // screen carrying the 3D composite and the capture -- out of batched mode
  // with it, 16 times a frame. DS_2D_SPLIT=1 enables the split; without it the
  // two entries are kept in lockstep and the behaviour is the old one.
  bool per_line_[2] = {false, false};
  bool per_line_prev_[2] = {false, false};
  u32  render_next_[2] = {SCREEN_H, SCREEN_H};
  bool split_ = false;            // DS_2D_SPLIT=1
  bool frame_finished_ = false;   // frame_done() called for both engines
  bool trap_armed_ = false, trap_lcdc_ = false;
  // Capture frames batch too (DS_2D_LAZY_CAPTURE=0 keeps them per line; see
  // the class comment). A trapped store catches the frame up, lifts the trap and renders the
  // next LAZY_BURST_LINES lines per line (exact without any trap, and no
  // slow-path stores while a DMA streams), then re-arms and batches again.
  // Two page-table walks per burst rather than two per line, and no trapped
  // store inside it. Past LAZY_BURST_LIMIT bursts the frame stays per line.
  bool lazy_capture_ = true;
  bool burst_[2] = {false, false};   // per-line for the current burst of stores
  u32  burst_left_[2] = {0, 0};      // display lines left before re-batching
  u32  lazy_bursts_[2] = {0, 0};
  static constexpr u32 LAZY_BURST_LIMIT = 16, LAZY_BURST_LINES = 8;
  // A frame in which both engines ended up per line paid for the trap and got
  // nothing: the batch never reached line 191. After LAZY_FUTILE_LIMIT of
  // those in a row the trap stops being armed at all, and one frame in
  // LAZY_PROBE_PERIOD re-arms it so a scene that starts batching again is
  // picked back up. Golden Sun's title is the case: an HBlank DMA rewrites
  // engine B's BG VRAM every scanline, so it is per line by nature, and the
  // trap cost 5.8 % of its frame to discover that 192 times a frame.
  static constexpr u32 LAZY_FUTILE_LIMIT = 4, LAZY_PROBE_PERIOD = 64;
  u32  lazy_futile_ = 0;
  bool lazy_tried_ = false;
  // A frame that spent its burst budget is futile whatever the engines'
  // state at line 191 (with DS_2D_SPLIT the other engine may still be
  // batching): counted at the fallback. A probe frame gets a smaller budget
  // (LAZY_PROBE_BURSTS) and falls both engines back at once when it runs
  // out, so re-checking a per-line scene costs half the arms it used to.
  static constexpr u32 LAZY_PROBE_BURSTS = 8;
  bool lazy_limit_hit_ = false, lazy_probe_ = false;
  u32  frontier() const { return hblank_done_ ? line_ + 1u : line_; }   // first line a write now can still affect
  void catch_up(u32 mask);             // render the masked engines' lines below the frontier
  void fall_back_per_line(u32 mask);   // catch up and render the rest of the frame per line
  void arm_trap();
  void disarm_trap();
  // Per-engine ranges; first > last means "nothing pending for that engine".
  // Engine B's range goes to the worker and overlaps engine A's here.
  void render_ranges(u32 af, u32 al, u32 bf, u32 bl);
  // Which engines a VRAM store can change: bit 0 engine A, bit 1 engine B.
  // Only the fixed BG/OBJ address ranges are attributed; anything else (LCDC,
  // an unmapped alias) is charged to both, so the split can only ever be more
  // conservative than the address map.
  u32 store_engines(u32 addr) const;
  void step_engine(int e, u32 line);        // one engine's display line: replay, latches, render, output

  // One worker thread beside the emulation thread, drawing a run of one
  // engine's display lines (worker_job). Which engine depends on the frame.
  //
  // A batched frame hands engine A's 192 lines over at the last display
  // line's HBlank and does not wait for them there: A carries the 3D
  // composite, the display capture and its half of the frontend scaling --
  // the expensive screen -- while engine B's batch is drawn here. The join
  // is deferred to the start of line 0, so the compositor overlaps the
  // VBlank period's emulation. Everything the guest can do meanwhile that
  // the lines in flight could observe joins earlier: a store into VRAM
  // engine A reads (the write trap stays armed until the join), a VRAMCNT
  // remap, a read of the bank a batched capture is writing (a read trap on
  // that bank, Bus::set_lcdc_read_trap), a full journal, a save state.
  // Register, palette and OAM writes during VBlank go through the journal
  // instead of the render side, as do the VBlank lines' own latches
  // (Engine2D::latch), and are applied in order at the join.
  //
  // Per-line frames (capture per line, the display FIFO, a VRAM trap) run
  // engine A here and hand engine B's lines to the worker, as before.
  //
  // The two engines share no mutable state -- the only statics they reach
  // are the read-only colour tables -- and no CPU runs inside the callback,
  // so a line sees exactly the register and VRAM state the sequential order
  // saw. DS_2D_THREAD=0 forces the sequential path for comparison.
  LineWorker worker_;
public:
  // DS_WATCHDOG: where the display pipeline stands when a frame stalls.
  void debug_dump(FILE* f);
private:
  int  job_e_ = 1;
  u32  job_first_ = 0, job_last_ = 0;
  bool par_2d_ = false;
  bool inflight_[2] = {false, false};   // that engine's lines are on the worker
  // What the lines read of the frame-level capture state, latched at each
  // render_ranges: DISPCAPCNT's enable bit clears itself at line 192 while
  // a batched engine A is still drawing.
  u32  capcnt_render_ = 0;
  bool capture_render_ = false;
  int  read_trap_bank_ = -1;            // LCDC bank under the capture read trap, or -1
  bool defer_join_ = true;              // DS_2D_DEFER=0: join engine A's batch at once (bisecting tool)
  Renderer3D::FrameRef ref3d_;          // the 3D frame these display lines read (begin_frame)
  // Per-line frames (capture, the display FIFO, a VRAM trap) hand engine B
  // one line at a time. Rather than wait for it at once -- which needs the
  // worker hot, i.e. spinning through the whole frame on a core the raster
  // workers want -- the line is left in flight and joined a line later
  // (inflight_[1]), so the worker can park between lines with no cost to the
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
  bool lag_frame_ = false;        // this frame's per-line lines may stay in flight
  u32  lag_trap_hits_ = 0;
  static constexpr u32 LAG_TRAP_LIMIT = 4096;   // GSDD traps ~55 stores a frame in capture frames; 64 dropped the lag every frame
  // Wait for whatever is on the worker. Engine A's deferred batch also ends
  // its frame here (finish_a): the journal drained -- the VBlank writes and
  // latches so far, in order -- then frame_done and the traps lifted, as
  // render_ranges does for a frame that finished on this thread.
  void join_worker();
  void finish_a();
public:
  void journal_full() { join_worker(); }   // Engine2D::queue on a full journal
private:
  static void worker_job(void* self);

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
