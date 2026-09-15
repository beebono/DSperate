// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS_FASTMEM_CENSUS=1: what mapping guest memory into host address space would
// meet, measured on the running workload before any of it is built (the
// fastmem scope's phase 0). A measurement instrument; nothing reads it back.
//
// Access census (interpreter runs, --interp): every data access is classified
// the way the planned host views would treat it -- direct, or a fault and why
// -- and the planned fault policy is simulated per access site (the guest
// instruction): the first fault rewrites the site to today's page-table walk
// for good, so its later accesses count as walked, not direct.
//
// Churn census (any run, JIT included): page-table entry changes outside the
// VRAM region (VRAM stays out of the views, so its traps are free), code-tag
// set and clear, which a view must mirror with mmap / mprotect.
#pragma once
#include "core/types.h"

namespace ds {
struct CpuContext;
}

namespace ds::mem::fmc {

extern bool g_on;              // DS_FASTMEM_CENSUS, read once at startup
inline bool on() { return g_on; }
void init();                   // reads the environment; call once before running

// The interpreter's data accesses (cpu_mem.h), after alignment.
void access(CpuContext& cpu, u32 addr, bool store);
// The interpreter's instruction fetches: the approximate code tags (see the .cpp).
void fetch(CpuContext& cpu, u32 addr);
// Page-table writes: `guest_page` whose entry changed from `old_e` to `new_e`.
void entry_changed(u32 guest_page, uintptr_t old_e, uintptr_t new_e);
// JIT code tag on one host page (both CPUs' tables follow through entry_changed).
void code_tag(bool on);

// Counting window: accesses and churn outside it are not counted, but the
// per-site rewrite state persists (a site rewritten in warm-up stays rewritten).
void set_counting(bool on);
void report(u64 frames);

} // namespace ds::mem::fmc
