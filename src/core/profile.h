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
  R3D_CLEAR, R3D_SPANS, R3D_FINAL, SPU,
  COUNT
};
extern bool enabled;
extern u64 ns[COUNT];
extern const char* const names[COUNT];
// Event counters (reported with the stages): how much work the stages did.
enum Counter : u32 { C_POLY_LINES, C_SPAN_PIXELS, C_RESOLVED_PIXELS, C_TEX_FAST, C_TEX_SLOW_FMT5, C_TEX_SLOW_VIEWS,
  C_TEXCACHE_HIT, C_TEXCACHE_DECODE, C_TEXCACHE_BYTES, C_R3D_FRAMES_KEPT, C_COUNT };
extern u64 count[C_COUNT];
extern const char* const count_names[C_COUNT];
inline void add(Counter c, u64 n) { if (enabled) count[c] += n; }
void report();

struct Scope {
  Stage s; std::chrono::steady_clock::time_point t0;
  explicit Scope(Stage st) : s(st) { if (enabled) t0 = std::chrono::steady_clock::now(); }
  ~Scope() { if (enabled) ns[s] += static_cast<u64>((std::chrono::steady_clock::now() - t0).count()); }
};

} // namespace ds::prof

#define DS_PROF(stage) ::ds::prof::Scope ds_prof_scope_##stage(::ds::prof::stage)
