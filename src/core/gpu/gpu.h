#include <atomic>
#include <vector>
// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstdio>
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/line_worker.h"
#include "core/gpu/render3d.h"
#include "core/gpu/colour.h"
#include "core/profile.h"

#include <array>
#include <memory>

namespace ds { struct NDS; }

namespace ds::gpu {

// Display timing, the two 2D engines and the output stage. 3D plugs in via
// Engine2D::set_3d_line().
//
// Lazy 2D: engines aren't rendered every HBlank. Writes are journaled per
// engine with the display line they first affect; the frame renders in one
// batch at the last line's HBlank, replaying the journal per line. A trapped
// VRAM store, VRAMCNT remap, or captured LCDC bank catches lines up and
// forces a per-line burst before re-batching.
//
// Framebuffers: 256x192 u32 per screen, 0xAARRGGBB, 8-bit channels expanded
// from the hardware's 6 bits.
class Gpu {
public:
  explicit Gpu(NDS& nds);
  ~Gpu();
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
  u64  probe_hash_ = 0, probe_vramcnt_at_l0_ = 0, probe_vramcnt_base_ = 0;   // latches POWCNT/FIFO/capture state; runs at line 0
  bool frame_begun() const { return frame_begun_; }
  bool at_line_start() const { return !hblank_done_; }

  // Save states, taken at line 0. quiesce() joins the worker and drains
  // journals without changing guest state; prepare_load() also lifts the VRAM
  // trap; after_load() re-takes the lazy/trap decision as begin_frame() did.
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

  // Journal stamp for a write to engine `e`: twice the first display line it
  // can affect, plus one if that line's scanline start already passed.
  // NO_STAMP means the write applies at once (nothing in flight, frame done).
  static constexpr u32 NO_STAMP = 0xFFFF;
  u32 journal_stamp(int e) const {
    const u32 l = hblank_done_ ? line_ + 1u : line_;
    return (l < SCREEN_H || inflight_[e]) ? l * 2 + (hblank_done_ ? 0 : 1) : NO_STAMP;
  }
  // Slow-path stores (Bus): a VRAM store on a trapped page catches the render up.
  void palette_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void oam_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void vram_store_trap(Cpu cpu, u32 addr);
  // After vram_store_trap: nothing in flight and both engines per-line, so the rest of a
  // DMA run on that page may write straight through a lag-mode trap (no hand-off until
  // the next HBlank event, which cannot fire inside a run).
  bool vram_trap_settled() const { return !inflight_[0] && !inflight_[1] && per_line_[0] && per_line_[1]; }
  bool vram_remap_begin(u32 moved_2d, const VramMap* next = nullptr);  // before a VRAMCNT remap: catch up, lift the trap; returns whether it was set (`next`: the map being switched to)
  void vram_remap_end(bool trapped);  // after: re-arm it
  void set_lazy(bool on) { lazy_enabled_ = on; }   // off: per-line rendering (tests)

  const u32* framebuffer(int screen) const { return pages_[front_].fb[screen].data(); }   // 0 = top, 1 = bottom
  // For a frontend that shows the frame from another thread: between frames, hands over the frame just finished
  // and moves the core's drawing to its other page, so what framebuffer() and split3d_screen() return from here
  // on is not written while the next frame is drawn (nothing to copy). It is written again by the frame after
  // that, and only once hand_frame has been called for the next one: the reader must be done by then.
  // The picture is what one buffer would hold: lines no frame has drawn since the pages changed over come from
  // the other page, at the next frame's start, from where readers get that frame whether it is handed or not.
  // No effect while the output stage scales into frontend buffers.
  void hand_frame();
  // Colour correction of every shown line (core/gpu/colour.h), after the
  // output stage and before the scaler or framebuffer, so fb_ holds the
  // corrected colours. Joins any lines in flight; set between frames.
  void set_colour_profile(ColourProfile p);
  ColourProfile colour_profile() const { return colour_; }
  // The profile's tables, null when off. A presenter that corrects the
  // frames itself (the Vulkan present, on its own thread) turns the
  // output stage's pass off, so fb_ keeps the game's colours. Joins on a change.
  const std::shared_ptr<const kern::ColourLut>& colour_lut() const { return colour_lut_; }
  void set_colour_on_cpu(bool on);
  bool colour_on_cpu() const { return colour_lut_ && colour_cpu_; }
  // The next drawn frame as the output stage gave it before colour
  // correction, for screenshots: request between frames, then after each
  // frame poll raw_frame_ready(). raw_frame() holds until the next request;
  // release_raw_frame() stops the copy if the caller gives up waiting.
  void request_raw_frame();
  bool raw_frame_ready() const { return raw_active_ && raw_lines_[0] >= SCREEN_H && raw_lines_[1] >= SCREEN_H && !lines_in_flight(); }
  const u32* raw_frame(int screen) const { return raw_fb_[screen].data(); }
  void release_raw_frame() { raw_active_ = false; }

  // The 3D split for a compositor that holds the GPU raster's image (the
  // Vulkan present; core/gpu/split3d.h). On: a frame drawn by the GPU raster
  // composes engine A's 3D lines without BG0, never reads the record (no
  // wait on the GPU, no lag rule), and the framebuffer of that screen holds
  // the picture without the 3D layer. A line that display-captures still
  // reads the record, the frame's own, and a capture of the screen merges
  // the line on the CPU for the game (the present merges it again for the
  // panel). Frames that scale or colour-correct on the CPU,
  // and lines with the layer scrolled, are composed in one pass as ever.
  // Applies from the next frame.
  void set_split3d_present(bool on) { split_req_ = on; }
  // A captured image the game copies about (Golden Sun: Dark Dawn: the bank
  // into main RAM with one DMA, then back a line per HBlank into a one-line
  // background on the other engine) keeps its capture tags, so the copy is
  // merged as the capture is. Dma calls dma_copied when an ARM9 channel has
  // moved `bytes` upwards from `src` to `dst`, if cap_follows().
  bool cap_follows() const { return split_engine_ && split_cap_ && cap_follow_; }
  void dma_copied(u32 src, u32 dst, u32 bytes);
  // After a frame: whether it was split, and what the merge needs. `words`:
  // 256x192, the split's word per pixel (0 on lines not split); `lines`: 192
  // words, master brightness as o * m + n >> 4 per 6-bit channel (m | n << 8);
  // `gen`: the GPU frame (Renderer3D::gpu_layer).
  // A screen showing a captured 3D image again is split the same way (see
  // split3d.h): `live` false, `gen` the frame that was captured, `coords`
  // the second word per pixel where any word has XY (else null).
  struct Split3DScreen { const u32* words = nullptr; const u32* coords = nullptr; const u32* lines = nullptr; u64 gen = 0; bool live = false; bool scrolled = false; };   // scrolled: some line has BG0's X offset (bits 20-28 of its `lines` word)
  bool split3d_screen(int screen, Split3DScreen* f) const;
  // The same merge on the CPU, from the frame's own record (waited for): the
  // split screen as the one-pass composite would have left it, into `dst`
  // (256x192). resolve_split3d() does so into the framebuffer itself, after
  // which the frame is no longer split: for every reader of framebuffer()
  // that is not that compositor. Between frames only.
  // A screen showing a capture needs no resolving: its framebuffer is the
  // 1x picture already, and merge_split3d gives the merge's result for it
  // (the same pixels at 1x: what DS_SPLIT3D=present counts).
  void merge_split3d(int screen, u32* dst);
  // The same merge at the raster's own scale N, from the frame's full-scale record (DS_LEAN_FINE=1), into `dst`
  // (256N x 192N): each 2D pixel N x N times, the 3D layer at every sub-pixel; what the GPU present shows at N x
  // (a picture tool: headless --dump-screens-scaled). False, with dst untouched, when that record is not there.
  // A screen not split is its framebuffer repeated. Between frames only, before resolve_split3d().
  bool merge_split3d_scaled(int screen, u32 n, u32* dst);
  void resolve_split3d();

  // A screen the frontend does not show. Its driving engine skips drawing but
  // keeps journal/latches/windows/lazy-2D bookkeeping running so it's exact
  // once shown again. Engine A never skips. Set between frames only.
  void set_screen_visible(int screen, bool on) { screen_visible_[screen] = on; }

  // Frameskip (frontend policy). A skipped frame runs unchanged, only
  // omitting line rendering/output and the 3D raster feeding it. Decided one
  // frame ahead, at line 215. Never skips a frame that captures or feeds the FIFO.
  void set_frame_skip(bool on) { skip_req_ = on; }
  // Skip frames that display-capture too (INEXACT): a game reading captured
  // pixels back with the CPU sees an older frame than hardware.
  void set_frameskip_capture(bool on) { skip_capture_ok_ = on; }

  // Frames until the display setup repeats. Drawing at a multiple of this
  // period freezes one screen on its off-frame, so keep the drawn cadence off
  // a multiple of it (1 = no alternation).
  u8 display_phase_period() const { return phase_period_; }
  bool lines_in_flight() const { return inflight_[0] || inflight_[1] || scale_inflight_; }
  bool lag_active() const { return lag_frame_ && !lazy_frame_; }
  // Bus::vram_read on an LCDC page under the capture read trap: join in-flight lines if reading the bank capture writes.
  bool lcdc_read_trapped() const { return read_trap_bank_ >= 0; }
  // A guest read reached the capture's bank through a mapping a remap gave
  // it while the capturing job is in flight: finish the job first.
  void capture_read_hit();
  // `force` false: not while a late capture into that bank waits for late_fill (the trap then stands until the
  // lines are written again: a read of them comes through capture_read_hit, which does it).
  void release_read_trap(bool force = false);   // whichever form the capture read trap is in
  bool vram_remap_begin_core(u32 moved_2d, const VramMap* next);
  void lcdc_read_hit(u32 addr) {
    if (static_cast<int>((addr >> 17) & 7) == read_trap_bank_) { prof::add(prof::C_2D_A_JOIN_READS, 1); join_worker(JoinSite::Trap); }
  }
  bool will_skip_frame() const { return skip_frame_; }

  // A frontend-owned, panel-sized destination for one screen. When set, the
  // output stage scales each line into it as produced instead of filling fb_.
  // Chunky with a cell of P panel pixels: cells_x * cells_y cells, each an
  // area-weighted box of the DS pixels it covers. Per axis, cell i takes taps
  // first[i]..first[i]+n[i]-1 with weights w[i][..] summing to 256.
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
    u8 chunky = 0;              // 0 off; else 2x2 block -> one cell: 1 top-left, 2 mean, 3 dominant colour,
                                // 4 darkest, 5 brightest, 6 farthest-from-mean luma beyond chunky_thresh (else mean)
    u32 chunky_thresh = 180 * 256;  // luma units (0..255 * 256); mode 6 only
    u8 blend = 0;               // box-filter seams: 1 blend in sRGB, 2 in linear light
    const u8* seam_w = nullptr; // 256 entries: weight (0..255 = 0..1) of pixel s+1 in run s's last pixel -- the part of
                                // that pixel past the s|s+1 boundary, 1 - frac((s+1)*w/256); 0 = no straddle
    const CellMap* cells = nullptr; // chunky with a panel-sized cell (see CellMap); null = the 2x2 pair path
    // Bilinear: blends the 2x2 source pixels around each sample point; takes
    // precedence over grid/seams/chunky. Column x samples between lin_sx[x]
    // and lin_sx[x]+1 at weight lin_wx[x]/256 (lin_sx <= 254).
    bool bilinear = false;
    const u16* lin_sx = nullptr;
    const u8* lin_wx = nullptr;
    // Rows of the rect, [y_lo, y_hi) (y_hi 0 = h: no crop); px points at row y_lo.
    u32 y_lo = 0, y_hi = 0;
  };
  // Both screens or neither: pass a null `px` to go back to fb_.
  void set_scale_target(int screen, const ScaleTarget& t) { scale_[screen] = t; if (scale_[screen].y_hi == 0) scale_[screen].y_hi = t.h; }
  // Runs a whole DS-resolution image (e.g. a frontend pause menu) through the
  // scanline scaler with the game's grid/chunky/seam treatment. No-op without a scale target.
  void scale_image(int screen, const u32* src);
  bool scaling() const { return scale_[0].px && scale_[1].px; }
  u16 line() const { return line_; }
  // A VCOUNT write (GBATEK: only during lines 202..212): the next scanline is
  // `v` instead of line_ + 1. Wireless games use it to line their frames up
  // with the host's. Never pending at line 0, where states are taken.
  void request_vcount(u16 v) { next_vcount_ = v; }

  Engine2D engine[2];

  // Both screens forced white by MASTER_BRIGHT (mode 1, full factor). Reading
  // the register beats scanning fb_ (pixels go white a frame later there).
  bool screens_forced_white() const {
    for (u16 mb : master_bright_g_)
      if ((mb >> 14) != 1 || (mb & 0x1F) < 16) return false;
    return true;
  }

private:
  NDS& nds_;
  u16 line_ = 0;
  u16 next_vcount_ = 0xFFFF;   // a VCOUNT write waiting for the next scanline
  bool hblank_done_ = false;  // this line's HBlank event has run (its render, if any, is behind us)
  bool frame_begun_ = false;
  bool screens_on_ = false;   // POWCNT1 bit 0, latched at frame start
  bool screen_visible_[2] = {true, true};
  bool skipped_[2] = {false, false};   // this engine's last line was skipped: its next drawn line re-renders its sprites
  bool skip_req_ = false;     // frameskip: the frontend's request, taken at line 215
  bool skip_next_ = false;    // taken there: the next frame's display lines are skipped
  bool skip_frame_ = false;   // latched in begin_frame from skip_next_, gated by skippable()
  // Never skip a frame the guest reads back; capture_recent_ extends that a
  // few frames after the last capture too (3D raster skips a frame ahead of the display it feeds).
  static constexpr u8 CAPTURE_STICKY = 8;
  u8 capture_recent_ = 0;
  bool skip_capture_ok_ = false;
  bool skippable() const { return !run_fifo_ && (skip_capture_ok_ || (!capture_on_ && !capture_recent_)); }
  // Display-phase detection: signature of the last PHASE_HISTORY frames, newest last.
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
  // What a frame leaves for its readers, twice over (hand_frame): the screens, and the 3D split's words, XY
  // words, per-line brightness words (Split3DScreen::lines) and brightness registers.
  struct Page {
    std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb{};
    std::vector<u32> split_fb[2], split_xy[2];   // 256x192 each (allocated on first use)
    u32 split_mb[2][SCREEN_H] = {};
    u16 split_reg[2][SCREEN_H] = {};
  };
  Page pages_[2];
  u32 page_ = 0, front_ = 0;            // where lines are drawn; what the readers between frames get
  u64 page_drawn_[2][3] = {};           // per screen, a bit a line: drawn into pages_[page_] since it became the drawing page
  Page& wp() { return pages_[page_]; }
  Page& fp() { return pages_[front_]; }
  const Page& fp() const { return pages_[front_]; }
  void patch_page();                    // the drawing page's undrawn lines, from the front page
  std::shared_ptr<const kern::ColourLut> colour_lut_;   // null: off
  ColourProfile colour_ = ColourProfile::Off;
  bool colour_cpu_ = true;
  // request_raw_frame: each engine's lines 0..191 copied once, from its first line 0 on.
  bool raw_active_ = false;
  u32 raw_lines_[2] = {};
  std::vector<u32> raw_fb_[2];
  ScaleTarget scale_[2];
  static constexpr u32 SCALED_ROW_MAX = 4096;
  alignas(16) u32 chunk_even_[2][SCREEN_W];   // chunky: the even line, held until the odd one completes the block
  alignas(16) u32 seam_prev_[2][SCREEN_W];    // blend: the previous source row, for the straddling row
  u32 seam_prev_line_[2] = {~0u, ~0u};
  // Cell chunky: last CELL_TAPS source lines, a ring by line number.
  alignas(16) u32 cell_lines_[2][CELL_TAPS][SCREEN_W];
  u32 cell_row_[2] = {0, 0};        // the cell row being gathered
  void emit_cells(int screen, u32 line, const u32* src);
  void emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst);
  // Bilinear: previous and current source lines widened horizontally, per
  // screen; a destination row between them is a lerp of the pair.
  alignas(16) u32 lin_row_[2][2][SCALED_ROW_MAX];
  u32 lin_cur_[2] = {0, 0};              // which of lin_row_[screen] holds the current line
  u32 lin_prev_line_[2] = {~0u, ~0u};
  void emit_bilinear(int screen, u32 line, const u32* src);
  void blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out);
  // output_line writes here instead of fb_ when scaling; scale_row reads it back hot.
  alignas(16) std::array<std::array<u32, SCREEN_W>, 2> line_out_{};
  // Scaled row staged here before the target (scanout memory, uncached CMA):
  // duplicating straight out of it would read that memory back.
  alignas(16) u32 row_scratch_[2][SCALED_ROW_MAX];
  const u32* line3d_ = nullptr;   // 3D output for the line being drawn (whichever thread draws engine A)

  // Lazy-2D state for the frame in progress.
  bool lazy_enabled_ = true;
  bool lazy_frame_ = false;       // this frame may batch
  // Per engine: a trapped store takes only the engine its address reaches out
  // of batched mode (a store into one engine's BG/OBJ VRAM cannot change what
  // the other fetches).
  bool per_line_[2] = {false, false};
  bool per_line_prev_[2] = {false, false};
  u32  render_next_[2] = {SCREEN_H, SCREEN_H};
  bool frame_finished_ = false;   // frame_done() called for both engines
  bool trap_armed_ = false, trap_lcdc_ = false;
  u32  trap_mask_ = 0;            // windows trapped: bit 0 engine A's, bit 1 engine B's (Bus::set_vram_trap)
  // Per-job VRAM map snapshots (0: worker_'s job, 1: worker_b_'s): a batch
  // renders lines the panel already scanned against the map as it was when
  // the job was handed, so a remap can rebuild the live map without joining
  // -- when the write trap still covers every bank the job reads
  // (vram_remap_begin). Copied only when the map's generation changed.
  VramMap job_map_[2];
  u32  job_map_gen_[2] = {~0u, ~0u};
  bool remap_pending_[2] = {false, false};   // engine's vram_remapped() owed at the join
  // A remap went through under a job in flight: the job reads the map as it
  // was, so a trapped store is taken as reaching every in-flight engine
  // (reach_engines answers for the live map's windows). Cleared at the join.
  bool snapshot_stale_ = false;
  void snapshot_map(int k);
  // Bank snapshot for a deferred batch (DS_2D_SNAP=0 turns it off). Games that
  // stream VRAM in VBlank store into what the batch is still reading, and the
  // store used to wait for the whole batch. While that keeps happening the
  // batch is handed a copy of the banks its engines read, so the store only
  // lifts the trap and lands. Not for a frame that captures or displays an
  // LCDC bank (those write or read the live bank from the job).
  static constexpr u32 SNAP_HOLD = 8;        // frames a snapshot is kept up after a store last met a busy batch
  u32  snap_want_ = 0;
  bool snap_hit_ = false;                    // this frame: a store reached a deferred batch still drawing
  bool snap_live_ = false;                   // the deferred jobs in flight read the snapshot
  std::unique_ptr<u8[]> snap_mem_;
  u8*  snap_bank_[9] = {};
  void take_snapshot(u32 engines);
  // Engines an address can change: bit 0 A, bit 1 B; LCDC only A (and only when trapped); else both.
  u32 reach_engines(u32 addr) const {
    if ((addr >> 24) != 0x06) return 3;
    if (addr >= 0x06800000) return trap_lcdc_ ? 1 : 0;
    return ((addr >> 21) & 1) ? 2 : 1;
  }
  // Capture frames batch too. A trapped store renders LAZY_BURST_LINES per
  // line, then re-arms; past LAZY_BURST_LIMIT bursts the frame stays per line.
  bool burst_[2] = {false, false};   // per-line for the current burst of stores
  u32  burst_left_[2] = {0, 0};      // display lines left before re-batching
  u32  lazy_bursts_[2] = {0, 0};
  static constexpr u32 LAZY_BURST_LIMIT = 16, LAZY_BURST_LINES = 8;
  // Per-engine futility (restored with the split from 508b7bc; the both-engines
  // skip of 8bdcd13 it replaces never fired once stores were charged per engine,
  // across 46 titles). An engine that ended LAZY_FUTILE_LIMIT frames in a row
  // per line starts its frames per line, bar a re-probe every LAZY_PROBE_PERIOD
  // frames; both futile means the frame does not batch. Golden Sun streams
  // engine B's BG every scanline (HBlank DMA): B goes per line, engine A --
  // 3D composite and capture -- keeps its batch. The trap follows (arm_trap):
  // only the windows of a batching engine, and A's in a lag frame.
  static constexpr u32 LAZY_FUTILE_LIMIT = 4, LAZY_PROBE_PERIOD = 64;
  u32  eng_futile_[2] = {0, 0};
  // Engine A's batch handed in pieces. A game that takes the frame's picture as soon as the last line is
  // drawn (Golden Sun: Dark Dawn copies each capture out of its bank by DMA at line 192) joins the batch the
  // moment it is handed, and the emulation waits out all 192 lines of it. While that goes on (early_a_: frames
  // left since such a join) the lines are handed as the frame goes, EARLY_A_LINES or more at a time and only
  // when the piece before is done (one waiting for the frame's 3D holds the rest back, never the emulation),
  // left in flight as a lag frame's line is (the trap over A's windows guards them): only the last piece is waited for. Not in
  // a frame whose 3D may be the frame before's (the record would be asked for at the first piece, before the
  // raster has had its time). DS_2D_EARLY=0: off; 2: in every frame that may (debug).
  static constexpr u32 EARLY_A_LINES = 16, EARLY_A_HOLD = 60;
  u32 early_a_ = 0;
  bool early_ok_ = false;        // this frame (begin_frame)
  bool early_inflight_ = false;  // a piece is in flight: engine B's lines are drawn beside it, with no join
  u32  eng_probe_in_[2] = {LAZY_PROBE_PERIOD, LAZY_PROBE_PERIOD};
  bool lazy_tried_ = false;       // last frame batched (its end state feeds eng_futile_)
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
  void step_engine(int e, u32 line);        // one engine's display line: replay, latches, render, output

  // One worker thread beside the emulation thread, drawing a run of one
  // engine's display lines (worker_job); which engine depends on the frame.
  //
  // A batched frame hands engine A's 192 lines over at the last line's
  // HBlank without waiting (A carries the 3D composite, capture and half of
  // frontend scaling; B's batch draws here). Join is deferred to line 0.
  // Anything the guest does meanwhile that in-flight lines could observe
  // joins earlier: a trapped VRAM store, a VRAMCNT remap, a read of a
  // batched-capture bank, a full journal, a save state. VBlank writes and
  // latches go through the journal, applied in order at the join.
  //
  // Per-line frames (capture per line, display FIFO, a VRAM trap) run engine
  // A here and hand engine B's lines to the worker, as before.
  //
  // The two engines share no mutable state, so a line sees exactly the state
  // the sequential order would.
  LineWorker worker_;
  // Split mode (with the thread layout, or DS_2D_BWORKER=1): engine B's batch in
  // a frame whose engine A batch is deferred goes to its own worker (paired
  // with a 3D band on its core) instead of this thread, deferred to the same
  // join. Its frame end (frame_done) moves to that join too.
  LineWorker worker_b_;
  bool split_ = false;
  bool split3d_verify_ = false;   // DS_SPLIT3D=verify
  bool b_deferred_ = false;             // engine B's batch is in flight on worker_b_ past line 191
  u32  bjob_first_ = 1, bjob_last_ = 0;
  static void worker_b_job(void* self);
public:
  // DS_WATCHDOG: where the display pipeline stands when a frame stalls.
  void debug_dump(FILE* f);
private:
  // The job: per engine, a run of lines (first > last = nothing for it).
  u32  job_first_[2] = {1, 1}, job_last_[2] = {0, 0};
  bool inflight_[2] = {false, false};   // that engine's lines are on the worker
  bool a_deferred_ = false;             // engine A's whole-frame batch is in flight past line 191 (finish_a at the join)
  // Frame-level capture state latched at each render_ranges: DISPCAPCNT's
  // enable bit clears at line 192 while a batched engine A is still drawing.
  u32  capcnt_render_ = 0;
  bool capture_render_ = false;
  // Gpu::capture() runs on the WORKER; vram_remap_begin() joins before a
  // remap changes vram_map_, so latching lcdc_mask_render_ here keeps capture off the shared object.
  u32  lcdc_mask_render_ = 0;
  int  read_trap_bank_ = -1;            // LCDC bank under the capture read trap, or -1
  bool read_trap_generic_ = false;      // ... as Bus::set_bank_read_trap (every page the bank backs), after a remap
  bool read_trap_reapply_ = false;      // vram_remap_end re-applies it (generic) over the rebuilt table
  // Engine B's scaling, handed to the worker with engine A's lagged lines:
  // the join is only for buffer reuse and frame end. Lines drawn here in lag
  // mode stash their output (output_engine); the worker scales it after engine A's lines.
  struct StashedLine { u32 line; int screen; alignas(16) u32 px[SCREEN_W]; };
  StashedLine bscale_[SCREEN_H];
  u32  bscale_n_ = 0;                   // lines stashed for the job being built / in flight
  bool bscale_defer_ = false;           // output_engine stashes engine B's line instead of scaling it
  bool scale_inflight_ = false;         // the worker holds a stash to scale
  Renderer3D::FrameRef ref3d_;          // the 3D frame these display lines read (begin_frame)
  // set_split3d_present: the frame being drawn and the last one finished.
  // want: lines may split; any: one did; drawn: engine A drew a line (a
  // skipped frame leaves the one before it standing, split or not).
  // track: captures are tagged and captured images shown again are split.
  // Per screen: any line split; live (engine A's 3D of this frame) or a
  // capture's; the 3D frame it reads.
  struct SplitScreen { bool any = false, live = false, coords = false, scrolled = false; Renderer3D::FrameRef ref; };
  struct SplitFrame { bool want = false, track = false, drawn = false; SplitScreen s[2]; };
  // What display capture left in banks A-D (and what dma_copied followed into them or bank H: Engine2D::CAP_SLOTS), while tracked. Per 128 pixels of
  // a bank: the 3D frame they were captured from (0: none, or nothing a
  // merge could redo), where that capture's image lies, a sum of the pixels
  // written (to tell them changed since), and their split words: `uniform`
  // for all of them, or, with `each`, one a pixel in cap_words_. Lines of two
  // frames can stand in one bank (a game showing a bank while capturing into
  // it line by line). cap_ref_: the last few captured frames' references, by gpu_gen.
  struct CapSeg { u64 gen = 0; u16 base = 0, width = 256; u32 sum = 0, uniform = 0; bool each = false; };
  std::vector<CapSeg> cap_seg_;
  Renderer3D::FrameRef cap_ref_[4];
  u32 cap_ref_at_ = 0;
  std::vector<u32> cap_words_;
  // A stretch of main RAM a DMA filled from tagged pixels of a bank: the
  // tags per 128 pixels (CapSeg, `base` here the pixel's place in its
  // capture; gen 0: none) and their words where a segment has one a pixel.
  struct CapCopy { u32 addr = 0, bytes = 0; std::vector<CapSeg> seg; std::vector<u32> words; };
  CapCopy cap_copy_[2];
  u32 cap_copy_at_ = 0;
  bool cap_follow_ = true;
  u64 cap_copied_out_ = 0, cap_copied_in_ = 0;   // DS_SPLIT_STAT: tagged segments followed into RAM, and back into a bank
  bool vram_pixels_at(u32 addr, u32& bank, u32& idx) const;
  void split_capture_display(int e, u32 line);
  bool split_req_ = false, split_engine_ = false, split_cap_ = true;
  u64 split_miss_cpu_ = 0, split_scrolled_ = 0, split_miss_mixed_ = 0;
  u64 split_cap_effect_ = 0;   // DS_SPLIT_STAT: captured 3D pixels on top but under a colour effect
  bool stat_on_ = false;   // DS_SPLIT_STAT: 3D lines not split, by cause
  // The late capture: a capture frame of a raster drawing above 1x does not wait for its own 3D frame. It
  // captures with the 3D of the frame before (the lag rule) and keeps what each line was merged from (LateCap);
  // the next frame's render, which is the first that can show the bank, waits for the frame instead and writes
  // the lines again with its 3D (late_fill): the bank and the picture are then what the waiting capture gives,
  // and the raster has had two frames. Until then the bank holds the frame before's 3D, which only a game
  // that reads its capture back at once would see. Only while captures are tagged whole (the lines late_fill
  // can redo): cap_untagged_ counts captured lines with 3D in them and no tag (the compositor's count),
  // cap_clean_ the capture frames since one. DS_CAP_SYNC=1: every capture frame waits, as at 1x.
  struct LateCap {
    bool pending = false;
    u64 frame = 0;                 // the display frame that captured (late_seq_ at its begin_frame)
    u32 cnt = 0, lcdc = 0;         // DISPCAPCNT and the LCDC banks, as its render had them
    u32 bank = 0;                  // the bank it captures into (begin_frame)
    Renderer3D::FrameRef ref;      // its 3D frame
    u8 kind[SCREEN_H] = {};        // per line: 0 not captured, 1 the merged composite, 2 the 3D layer alone
    std::vector<Pixel> u;          // kind 1: the line without BG0 ...
    std::vector<u32> w;            // ... and its split words
  };
  LateCap late_[2];
  u64 late_seq_ = 0;
  Renderer3D::FrameRef ref3d_exact_;   // a late frame's own 3D frame, waited for: for a line that shows the 1x record itself
  bool late_hold(int bank) const { return (late_[0].pending && static_cast<int>(late_[0].bank) == bank) || (late_[1].pending && static_cast<int>(late_[1].bank) == bank); }
  int late_at_ = -1, late_render_ = -1;   // this frame's (begin_frame); as handed to the render
  bool late_fill(bool current = false);   // true: lines were written again; `current`: this frame's capture too, once its lines are all drawn
  bool cap_late_ = false, cap_late_ok_ = true;
  std::atomic<u32> cap_untagged_{0};
  // A late capture something took before the next frame's render (a read, a DMA out, a texture mapping) was
  // written again at once, which is the waiting capture with work added: late capture stands down for a while.
  u32 cap_backoff_ = 0;
  // DS_SPLIT_STAT: capture frames; those that waited for their 3D frame while late capture stood down; late captures taken early, by what took them.
  u32 cap_frames_ = 0, cap_backoff_frames_ = 0, cap_taken_remap_ = 0, cap_taken_read_ = 0, cap_taken_dma_ = 0;
  u32 cap_idle_ = 0;   // frames since the last capture frame: a new run of captures starts by waiting (cap_clean_ from 0)
  u32 cap_untagged_seen_ = 0, cap_clean_ = 0, cap_late_frames_ = 0, cap_late_filled_ = 0;
  u64 split_lines_ = 0, split_resolved_ = 0, split_cap_merged_ = 0;   // DS_SPLIT3D=present: what the run split and merged back
  SplitFrame split_cur_, split_done_;
  u64 split_cap_shown_ = 0, split_cap_moved_ = 0, split_cap_changed_ = 0, split_cap_other_ = 0, split_cap_bad_ = 0, split_cap_checked_ = 0;
  // Lag mode: per-line frames hand BOTH engines' line to the
  // worker at its HBlank without waiting, joining at that line's HBlank or
  // earlier if observed. The last display line always joins. Past
  // LAG_TRAP_LIMIT joining stores, or LAG_STORE_LIMIT trapped stores of any
  // kind (each is a slow-path store, whether or not it had to wait), in a frame,
  // lag is dropped and the trap lifted. Display FIFO frames stay on this thread.
  bool lag_frame_ = false;        // this frame's per-line lines may stay in flight
  u32  lag_trap_hits_ = 0, lag_trap_stores_ = 0;
  static constexpr u32 LAG_TRAP_LIMIT = 4096;
  static constexpr u32 LAG_STORE_LIMIT = 1024;
  // Wait for whatever is on the worker. Engine A's deferred batch also ends
  // its frame here (finish_a), as render_ranges does for a frame finished on this thread.
  enum class JoinSite { CatchUp, Trap, Journal, Line0, Remap, RangesPre, RangesPost, Other };   // where a join was taken from, to attribute its wait
  void join_worker(JoinSite site = JoinSite::Other);
  u64 join_wait_ns_ = 0;
  void finish_a();
public:
  void journal_full() { join_worker(JoinSite::Journal); }   // Engine2D::queue on a full journal
private:
  static void worker_job(void* self);

  void output_engine(int e, u32 line);
  void emit_scaled(int screen, u32 line, const u32* src);
  void output_a(u32 line, u32* dst);
  void output_b(u32* dst);
  void capture(u32 line, const Pixel* composite = nullptr, const u32* words = nullptr);   // words: the line's split words to tag the capture with (set_split3d_present), or null   // composite: engine A's line when it is not Engine2D::output() (a split line, merged)
  void apply_master_brightness(u16 reg, u32* dst);
  void expand_colours(u32* dst);
  bool uses_fifo() const;
  void sample_fifo(u32 offset, u32 count);
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2;   // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
