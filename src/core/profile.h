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
extern const char* const names[COUNT];
// Event counters (reported with the stages): how much work the stages did.
enum Counter : u32 { C_POLY_LINES, C_SPAN_PIXELS, C_RESOLVED_PIXELS, C_TEX_FAST, C_TEX_SLOW_FMT5, C_TEX_SLOW_VIEWS,
  C_TEXCACHE_HIT, C_TEXCACHE_DECODE, C_TEXCACHE_BYTES, C_R3D_FRAMES_KEPT,
  C_SLICES, C_SLICES_A9_HALTED, C_SLICES_A7_HALTED, C_SLICES_BOTH_HALTED, C_SLICES_DMA, C_SLICES_SKIPPED,
  C_CYC_TOTAL, C_CYC_BOTH_HALTED, C_CYC_A9_ONLY_HALTED, C_CYC_A7_ONLY_HALTED, C_CYC_NEITHER_HALTED,
  C_CYC_A9_SPIN, C_CYC_A7_SPIN, C_CYC_ONE_SPIN_ONE_HALTED, C_CYC_BOTH_SPIN_OR_HALTED,
  C_NS_A9_SPIN, C_NS_A7_SPIN, C_NS_A9_WORK, C_NS_A7_WORK, C_CYC_IDLE_SKIPPED, C_IDLE_NO_DMA, C_IDLE_NO_GX, C_IDLE_NO_IRQ, C_IDLE_NO_FILTER, C_IDLE_NO_LOOP9, C_IDLE_NO_LOOP7, C_IDLE_OK,
  C_2D_LINES, C_2D_BG_TEXT, C_2D_BG_AFFINE, C_2D_BG_EXT, C_2D_BG_3D, C_2D_OBJ_LINES, C_2D_WINDOW_LINES, C_2D_EFFECT_LINES, C_2D_EFFECT_LIVE, C_2D_FLAT_LINES, C_2D_SELECTS,
  C_2D_L0, C_2D_L1, C_2D_L2, C_2D_L3, C_2D_L4P, C_2D_L1_FULL, C_2D_OBJ_PRESENT, C_2D_3D_PRESENT, C_2D_WIN_PRESENT, C_2D_BG_PAL16, C_2D_BG_PAL256, C_2D_BG_DIRECT, C_2D_BG_EMPTY, C_2D_BG_3D_EMPTY, C_2D_FAST_BACKDROP, C_2D_FAST_ONE, C_2D_FULL_MODE, C_2D_FULL_3D, C_2D_FULL_OBJ, C_2D_FULL_SECOND, C_2D_FULL_FADE, C_SPAN_FLAT_RGB, C_SPAN_LERP_RGB, C_BAND0_NS, C_BAND1_NS, C_BAND2_NS, C_BAND3_NS, C_BAND_MAX_NS, C_BAND_SUM_NS, C_ASYNC_FRAMES, C_ASYNC_DIRTY_L0, C_ASYNC_DIRTY_SWAP, C_ASYNC_VRAMCNT_L0, C_ASYNC_VRAMCNT_SWAP, C_R3D_SYNC_ALL, C_COUNT };
extern const char* const count_names[C_COUNT];

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
