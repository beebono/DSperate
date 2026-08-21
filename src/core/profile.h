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
  R3D_CLEAR, R3D_SPANS, R3D_FINAL,
  COUNT
};
extern bool enabled;
extern u64 ns[COUNT];
extern const char* const names[COUNT];
void report();

struct Scope {
  Stage s; std::chrono::steady_clock::time_point t0;
  explicit Scope(Stage st) : s(st) { if (enabled) t0 = std::chrono::steady_clock::now(); }
  ~Scope() { if (enabled) ns[s] += static_cast<u64>((std::chrono::steady_clock::now() - t0).count()); }
};

} // namespace ds::prof

#define DS_PROF(stage) ::ds::prof::Scope ds_prof_scope_##stage(::ds::prof::stage)
