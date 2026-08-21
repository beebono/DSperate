// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM -> AArch64 recompiler: public interface. See README.md in this
// directory for the design; docs/ARCHITECTURE.md §3 for the CpuContext
// contract it relies on.
#pragma once
#include "core/cpu/cpu.h"

#include <cstdio>

namespace ds { struct NDS; }

namespace ds::jit {

struct Stats {
  u64 blocks_translated = 0;
  u64 instrs_translated = 0;   // guest instructions emitted inline
  u64 instrs_fallback = 0;     // guest instructions routed to the interpreter helper
  u64 blocks_invalidated = 0;
  u64 flushes = 0;
  u64 entries = 0;             // native entries (one per scheduler slice at most)
};

// Create the runtime (code arena, stubs) and route the selected CPUs through
// translated code. Safe to call once per NDS; `trace` mirrors NDS::trace at
// attach time (toggling tracing later requires `set_trace`).
bool attach(NDS& nds, bool arm9, bool arm7);
void detach(NDS& nds);

// RunFn entry: runs `cpu` until its budget is exhausted or it halts.
void run(CpuContext& cpu);

// Drop every translated block of `cpu` (timing tables changed, tracing
// toggled, debugger). Cheap to call; translation is lazy.
void flush(CpuContext& cpu);
void flush_all();

// Per-instruction tracing: when enabled, every translated instruction calls
// NDS::trace before executing, exactly as the interpreter does.
void set_trace(bool on);

const Stats& stats();
// Print the counters (and, with DS_JIT_HIST=1, the hottest fallback sites).
void report(std::FILE* out);

} // namespace ds::jit
