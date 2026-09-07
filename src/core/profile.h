// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <chrono>
#include <vector>

namespace ds::prof {

// Coarse stage timer for the headless builds (DS_PROFILE=1 in the headless
// frontend):
// wall time accumulated per stage, one branch of overhead when disabled.
enum Stage : u32 {
  CPU9, CPU7, DMA, GX_RUN,
  BG_DRAW, OBJ_DRAW, WINDOW, SELECT, EFFECTS, OUTPUT, CAPTURE,
  R3D_CLEAR, R3D_SPANS, R3D_FINAL, R3D_WAIT, SPU,
  // Nested inside CPU9/CPU7 (translation runs mid-slice), so it is an
  // "of which" column: never add it to the others against wall time.
  JIT_TX,
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
  C_2D_L0, C_2D_L1, C_2D_L2, C_2D_L3, C_2D_L4P, C_2D_L1_FULL, C_2D_OBJ_PRESENT, C_2D_3D_PRESENT, C_2D_WIN_PRESENT, C_2D_BG_PAL16, C_2D_BG_PAL256, C_2D_BG_DIRECT, C_2D_BG_EMPTY, C_2D_BG_3D_EMPTY, C_2D_FAST_BACKDROP, C_2D_FAST_ONE, C_2D_FULL_MODE, C_2D_FULL_3D, C_2D_FULL_OBJ, C_2D_FULL_SECOND, C_2D_FULL_FADE, C_SPAN_FLAT_RGB, C_SPAN_LERP_RGB, C_BAND0_NS, C_BAND1_NS, C_BAND2_NS, C_BAND3_NS, C_BAND_MAX_NS, C_BAND_SUM_NS, C_ASYNC_FRAMES, C_ASYNC_DIRTY_L0, C_ASYNC_DIRTY_SWAP, C_ASYNC_VRAMCNT_L0, C_ASYNC_VRAMCNT_SWAP, C_R3D_SYNC_ALL, C_R3D_STOLEN, C_R3D_W1, C_R3D_W2, C_R3D_W3, C_R3D_W4, C_BATCHES, C_BATCH_SPANS, C_BATCH_PX,
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
  // Lazy 2D: frames that started batched, and VRAM-trap hits (each one
  // drops a frame to per-line rendering).
  C_2D_LAZY_FRAMES, C_2D_LAZY_SKIPPED, C_2D_TRAP_HITS,
  C_2D_LAG_FRAMES, C_2D_LAG_STORES, C_2D_LAG_STORE_JOINS, C_2D_LAG_DROPPED, C_2D_LAG_LINES, C_2D_A_JOIN_STORES, C_2D_A_JOIN_READS,
  C_RESOLVE_CALLS, C_RESOLVE_PARTS,
  C_CHUNK_ENTRIES,
  C_SPAN_EMPTY, C_SPAN_OCCLUDED, C_SPAN_DRAWN,
  // Census: drawn spans/pixels by resolve reason -- what the Shade needed, not
  // which code ran, so the census reads the same on a build without NEON (see
  // resolve_span). On a NEON build `plain` and `toon/highlight` are both vector
  // stages in flush_batch, differing in the stages they run; only shadow and
  // wireframe force the scalar resolve_span. The labels used to call
  // toon/highlight scalar, which stopped being true when it was vectorised.
  // Census: geometry-engine register reads (the ARM9 polling GXSTAT).
  C_GX_READ, C_GX_READ_GXSTAT, C_GX_READ_GXSTAT_BUSY, C_GX_READ_GXSTAT_PIPE, C_GX_READ_GXSTAT_FIFO, C_GX_RUN_SLOW, C_GX_RUN_SLOW_EXEC,
  C_RES_VEC_SPANS, C_RES_VEC_PX, C_RES_TOON_SPANS, C_RES_TOON_PX, C_RES_SHADOW_SPANS, C_RES_SHADOW_PX, C_RES_WIRE_SPANS, C_RES_WIRE_PX,
  // Census: the per-scanline change-detection compares in engine2d.
  C_2D_CMP_BGPAL, C_2D_CMP_BGEXT, C_2D_CMP_OBJPAL, C_2D_CMP_OBJEXT, C_2D_CMP_OAM,
  C_2D_CMPD_BGPAL, C_2D_CMPD_BGEXT, C_2D_CMPD_OBJPAL, C_2D_CMPD_OBJEXT, C_2D_CMPD_OAM,
  // Census: DMA. `run` units moved through a direct-mapped page-to-page run
  // (one page-table walk per end per run); `slow` units through the bus, one
  // dispatch each -- the split says whether a title's DMA cost is memory or
  // dispatch. Starts are counted per ARM9 start mode, with the ARM7's lumped.
  C_DMA_STARTS, C_DMA_LOOP,   // C_DMA_LOOP: outer-loop entries; a run counts once, a per-unit step counts one each
  C_DMA_GXF_WORDS, C_DMA_GXF_SLOW, C_DMA_GXF_RUNS,
  C_DMA_RUN_SEGS, C_DMA_RUN_W, C_DMA_RUN_H, C_DMA_SLOW_W, C_DMA_SLOW_H,
  C_DMA_VRAM_TRAP,
  // Units by destination zone, and the VRAM traps a run took, by the same
  // zone: what the DMA is actually feeding, and which surface the lazy-2D
  // trap keeps firing on.
  C_DMA_D_MAIN, C_DMA_D_WRAM, C_DMA_D_PAL, C_DMA_D_OAM, C_DMA_D_IO, C_DMA_D_OTHER,
  C_DMA_D_BGA, C_DMA_D_BGB, C_DMA_D_OBJA, C_DMA_D_OBJB, C_DMA_D_LCDC,
  C_DMA_T_BGA, C_DMA_T_BGB, C_DMA_T_OBJA, C_DMA_T_OBJB, C_DMA_T_LCDC,
  // Render ranges issued per engine: batching means one a frame, per-line means one per line.
  C_2D_RANGE_A, C_2D_RANGE_B,
  C_DMA_M_IMM, C_DMA_M_VBLANK, C_DMA_M_HBLANK, C_DMA_M_DISPSTART, C_DMA_M_DISPFIFO, C_DMA_M_CART, C_DMA_M_GBA, C_DMA_M_GXFIFO, C_DMA_M_ARM7,
  // Census: how uniform the resolve's per-pixel kind decision actually is.
  // Every eight-pixel group builds a per-lane kind code (opaque / translucent
  // / translucent-over-a-pixel / the same two on the under layer) and branches
  // on the reduction. If groups and batches are overwhelmingly a single kind,
  // the reduction and its branches are pure overhead and the resolve wants
  // DraStic's answer -- a kernel chosen per polygon, not a test per group
  // (docs/techniques/02, the AND/OR uniformity test and the _constant family).
  // A group is UNIFORM when every drawing lane in it carries the same kind.
  C_RK_GROUPS, C_RK_EMPTY, C_RK_UNIFORM, C_RK_MIXED, C_RK_OPAQUE, C_RK_TRANS, C_RK_UNDER,
  C_RK_BATCHES, C_RK_BATCH_EMPTY, C_RK_BATCH_UNIFORM, C_RK_BATCH_MIXED, C_RK_BATCH_OPAQUE,
  // The same batch split weighted by drawing groups, because a big batch has
  // more chances to be mixed: counting batches alone flatters the uniform share.
  C_RK_BATCH_GRP, C_RK_BATCH_UNIFORM_GRP, C_RK_BATCH_MIXED_GRP,
  // Why a group drew nothing, split by what the pre-pass had said. UNDER: no
  // lane had pass bit 0, so the group only ever held under-layer candidates --
  // reachable from the pre-pass, which today defers that depth test. TOP: some
  // lane passed depth on the top layer and was then killed by the alpha test,
  // which cannot move into the pre-pass (colour does not exist until span_shade).
  C_RK_EMPTY_UNDER, C_RK_EMPTY_TOP,
  // Groups where EVERY lane draws and every lane is opaque -- the interior of
  // a fullscreen quad. Their destination loads and bsl selects are dead work:
  // the stores could be unconditional. FULL8 is a whole 8-lane group; FULLPX
  // counts its pixels so the share can be read against resolved pixels.
  C_RK_FULL_OPAQUE, C_RK_FULL_OPAQUE_PX,
  // JIT retimes (ARM9 timing-table rebuilds: PU / TCM / EXMEMCNT writes):
  // calls, and the blocks each one killed because a byte they baked changed.
  C_JIT_INVALIDATE_CPU, C_JIT_INVALIDATE_CPU_KILLED,
  // Slices the ARM9 sat out with the geometry FIFO full (the 128-cycle drain poll).
  C_SLICES_GX_STALLED,
  // Memory-map and timing-table rebuilds (each is a page-table or 1 MB
  // table walk): VRAMCNT remaps, TCM/PU window updates, EXMEMCNT slot
  // retimes, and ARM9 timing-range rebuilds from any of them.
  C_BUS_UPDATE_VRAM, C_BUS_UPDATE_TCM, C_BUS_GBA_TIMING, C_TIMING_UPDATE_CPU9,
  C_COUNT };
// Unbounded on purpose: profile.cpp defines it with a deduced size and
// static_asserts that size against C_COUNT. Declared as [C_COUNT] instead, a
// short initialiser list silently pads with nullptr and the report prints
// garbage for the missing tail -- which is exactly what a careless merge of two
// branches that each added a counter produces.
extern const char* const count_names[];

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

// Per-frame stage series: frame_mark() snapshots the stage accumulators at a
// frame boundary (call it where the frontend closes its frame_ms sample), and
// frame_breakdown() then answers the question the whole-run report cannot:
// what do the p99 frames spend their time on that the typical frame does not?
// A stage that is 2% of the run but 100% of the spikes is invisible in
// report() and is exactly what the tail is made of.
void frame_mark();
void frame_breakdown(const std::vector<double>& frame_ms);

struct Scope {
  Stage s; std::chrono::steady_clock::time_point t0;
  explicit Scope(Stage st) : s(st) { if (enabled) t0 = std::chrono::steady_clock::now(); }
  ~Scope() { if (enabled) add_ns(s, static_cast<u64>((std::chrono::steady_clock::now() - t0).count())); }
};

} // namespace ds::prof

#define DS_PROF(stage) ::ds::prof::Scope ds_prof_scope_##stage(::ds::prof::stage)
