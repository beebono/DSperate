// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <chrono>

namespace ds::prof {

// Coarse stage timer for the headless builds (DS_PROFILE=1 in the CLI):
// wall time accumulated per stage, one branch of overhead when disabled.
enum Stage : u32 {
  CPU9, CPU7, DMA, GX_RUN,
  BG_DRAW, OBJ_DRAW, WINDOW, SELECT, EFFECTS, OUTPUT, CAPTURE,
  R3D_CLEAR, R3D_SPANS, R3D_FINAL, R3D_WAIT, SPU,
  COUNT
};
extern bool enabled;
// DS_ASYNC_PROBE: true between the line the raster would start on and the
// deadline it would have to be joined by -- the window an async raster would
// be exposed to CPU writes in.
extern bool async_window;
// DS_CENSUS_GX: set at swap when the submitted list matched the previous one,
// so the rasteriser can charge the work it is about to redo to its own
// counters. Written on the emulation thread at vblank, read by the band
// workers during the raster that follows -- a census, not a synchronisation.
extern bool census_same_list;
extern const char* const names[COUNT];
// Event counters (reported with the stages): how much work the stages did.
enum Counter : u32 { C_POLY_LINES, C_SPAN_PIXELS, C_RESOLVED_PIXELS, C_TEX_FAST, C_TEX_SLOW_FMT5, C_TEX_SLOW_VIEWS,
  C_TEXCACHE_HIT, C_TEXCACHE_DECODE, C_TEXCACHE_BYTES, C_R3D_FRAMES_KEPT,
  C_SLICES, C_SLICES_A9_HALTED, C_SLICES_A7_HALTED, C_SLICES_BOTH_HALTED, C_SLICES_DMA, C_SLICES_SKIPPED,
  C_CYC_TOTAL, C_CYC_BOTH_HALTED, C_CYC_A9_ONLY_HALTED, C_CYC_A7_ONLY_HALTED, C_CYC_NEITHER_HALTED,
  C_CYC_A9_SPIN, C_CYC_A7_SPIN, C_CYC_ONE_SPIN_ONE_HALTED, C_CYC_BOTH_SPIN_OR_HALTED,
  C_NS_A9_SPIN, C_NS_A7_SPIN, C_NS_A9_WORK, C_NS_A7_WORK, C_CYC_IDLE_SKIPPED, C_IDLE_NO_DMA, C_IDLE_NO_GX, C_IDLE_NO_IRQ, C_IDLE_NO_FILTER, C_IDLE_NO_LOOP9, C_IDLE_NO_LOOP7, C_IDLE_OK,
  C_2D_LINES, C_2D_BG_TEXT, C_2D_BG_AFFINE, C_2D_BG_EXT, C_2D_BG_3D, C_2D_OBJ_LINES, C_2D_WINDOW_LINES, C_2D_EFFECT_LINES, C_2D_EFFECT_LIVE, C_2D_FLAT_LINES, C_2D_SELECTS,
  C_2D_L0, C_2D_L1, C_2D_L2, C_2D_L3, C_2D_L4P, C_2D_L1_FULL, C_2D_OBJ_PRESENT, C_2D_3D_PRESENT, C_2D_WIN_PRESENT, C_2D_BG_PAL16, C_2D_BG_PAL256, C_2D_BG_DIRECT, C_2D_BG_EMPTY, C_2D_BG_3D_EMPTY, C_2D_FAST_BACKDROP, C_2D_FAST_ONE, C_2D_FULL_MODE, C_2D_FULL_3D, C_2D_FULL_OBJ, C_2D_FULL_SECOND, C_2D_FULL_FADE, C_SPAN_FLAT_RGB, C_SPAN_LERP_RGB, C_BAND0_NS, C_BAND1_NS, C_BAND2_NS, C_BAND3_NS, C_BAND_MAX_NS, C_BAND_SUM_NS, C_ASYNC_FRAMES, C_ASYNC_DIRTY_L0, C_ASYNC_DIRTY_SWAP, C_ASYNC_VRAMCNT_L0, C_ASYNC_VRAMCNT_SWAP, C_R3D_SYNC_ALL, C_BATCHES, C_BATCH_SPANS, C_BATCH_PX,
  // Census: how often the 3D frame is resubmitted unchanged (DS_CENSUS_GX=1
  // adds the content hash, which is not free).
  C_GX_SWAP, C_GX_SWAP_SAME_CONTENT, C_GX_NOSWAP, C_GX_NOSWAP_REGS_DIFFER,
  C_GX_RD_DISPCNT, C_GX_RD_CLEAR, C_GX_RD_FOG, C_GX_RD_EDGETOON,
  C_GX_SWAP_POLYS, C_GX_SWAP_VERTS, C_GX_SWAP_MAXPOLYS, C_GX_SWAP_MAXVERTS,
  C_GX_CMP_FULL, C_GX_CMP_EARLY, C_GX_CMP_RUNS,
  C_GX_CMP_FULL_SAME, C_GX_CMP_RUNS_SAME, C_GX_CMP_FULL_DIFF, C_GX_CMP_EARLY_DIFF, C_GX_CMP_RUNS_DIFF,
  C_GX_SAME_POLYS, C_GX_SAME_VERTS,
  C_POLY_LINES_SAME, C_SPAN_PIXELS_SAME,
  // Span-length histogram: spans, and the pixels in them, by length bucket
  // (1-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129-256). Mean span hides how much
  // of the work sits in spans too short to amortise a vector preamble.
  C_SL0, C_SL1, C_SL2, C_SL3, C_SL4, C_SL5, C_SL6,
  C_SLPX0, C_SLPX1, C_SLPX2, C_SLPX3, C_SLPX4, C_SLPX5, C_SLPX6,
  // Stores landing in palette / OAM space: what a write-path dirty bit would
  // have to intercept, and therefore what it would cost to slow-path.
  C_W_PALETTE, C_W_OAM,
  C_RESOLVE_CALLS, C_RESOLVE_PARTS,
  // Census: the per-scanline change-detection compares in engine2d.
  C_2D_CMP_BGPAL, C_2D_CMP_BGEXT, C_2D_CMP_OBJPAL, C_2D_CMP_OBJEXT, C_2D_CMP_OAM,
  C_2D_CMPD_BGPAL, C_2D_CMPD_BGEXT, C_2D_CMPD_OBJPAL, C_2D_CMPD_OBJEXT, C_2D_CMPD_OAM,
  C_COUNT };
// Unbounded on purpose: profile.cpp defines it with a deduced size and
// static_asserts that size against C_COUNT. Declared as [C_COUNT] instead, a
// short initialiser list silently pads with nullptr and the report prints
// garbage for the missing tail -- which is exactly what a careless merge of two
// branches that each added a counter produces.
extern const char* const count_names[];

// The 3D raster runs on several band threads (docs/THREADED-RASTER.md), so the
// accumulators are per thread: a single set of globals put every worker's
// `+=` on one cache line, and the ping-pong showed up as time charged to the
// stage being measured. Each thread owns an Accum; `report` sums them.
struct Accum {
  u64 ns[COUNT] = {};
  u64 count[C_COUNT] = {};
  u32 tid = 0;          // registration order: 0 is the emulation thread, 1.. the band workers
};
namespace detail {
extern thread_local Accum* acc;
Accum* make_acc();                                  // registers a new one (once per thread)
inline Accum* get() { Accum* a = acc; return a ? a : make_acc(); }
}

inline void add(Counter c, u64 n) { if (enabled) detail::get()->count[c] += n; }
inline u64 count(Counter c) { return enabled ? detail::get()->count[c] : 0; }
inline void add_ns(Stage s, u64 n) { if (enabled) detail::get()->ns[s] += n; }
void report();

struct Scope {
  Stage s; std::chrono::steady_clock::time_point t0;
  explicit Scope(Stage st) : s(st) { if (enabled) t0 = std::chrono::steady_clock::now(); }
  ~Scope() { if (enabled) add_ns(s, static_cast<u64>((std::chrono::steady_clock::now() - t0).count())); }
};

} // namespace ds::prof

#define DS_PROF(stage) ::ds::prof::Scope ds_prof_scope_##stage(::ds::prof::stage)
