// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM -> AArch64 recompiler: public interface. See README.md in this
// directory for the design.
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
  u64 code_bytes = 0;          // native bytes emitted for blocks (cold sections included)
  u64 hot_bytes = 0;           // of which the hot sections (what runs on the fast paths)
  u64 slow_accesses = 0;       // loads/stores that left the inline page-table path
};

// Create the runtime (code arena, stubs) and route the selected CPUs through
// translated code. Safe to call once per NDS; `trace` mirrors NDS::trace at
// attach time (toggling tracing later requires `set_trace`).
bool attach(NDS& nds, bool arm9, bool arm7);
void detach(NDS& nds);

// RunFn entry: runs `cpu` until its budget is exhausted or it halts.
void run(CpuContext& cpu);

// Native slice loop:the scheduler's slice state machine
// (Scheduler::slice_next) is driven from a loop in the code arena
// that saves the callee-saved registers once and enters translated code
// without the per-entry frame. `lookup` is the block lookup that
// run() does before each entry (translating or resetting the arena as
// needed); `run_loop` returns when slice_next reports the end.
const void* lookup(CpuContext& cpu);
bool has_runtime();
void run_loop(void* scheduler);

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
