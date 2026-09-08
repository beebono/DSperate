// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Idle-loop detection: proving that a CPU sitting in a polling loop can be
// skipped to the next scheduled event instead of interpreted or recompiled.
//
// A loop is skippable only when executing N iterations is indistinguishable
// from executing none, so the analysis demands all of:
//
//   * no stores, no MMIO, no SWI/coprocessor/PSR writes, no calls -- nothing
//     that can be observed from outside the loop;
//   * every load comes from a page the page table backs with real RAM, with
//     no writeback, so the address is loop-invariant and side-effect-free;
//   * no loop-carried register dependency: the set of registers the body
//     writes is disjoint from the set it reads before writing. This is what
//     separates a poll loop (`ldr; tst; bne`) from a delay loop
//     (`subs r0,#1; bne`), which counts down and must not be skipped.
//
// The structural verdict is cached per (pc, mode); the load addresses are
// re-checked against the live registers every time, since the same code can
// be entered with a different base pointer.
#pragma once
#include "core/types.h"

namespace ds {
struct CpuContext;
}

namespace ds::cpu {

// Which addresses a loop's loads may touch. RamOnly: backed RAM. GxstatOnly:
// RAM plus GXSTAT, and at least one load must be GXSTAT (the swap-wait shape).
// All: RAM plus the scheduled-event register set (see safe_poll_address).
enum class IdlePorts : u8 { RamOnly, GxstatOnly, SpicntOnly, All };   // SpicntOnly: RAM and SPICNT (the ARM7's SPI busy poll)

// True when the CPU's next instruction lies inside a proven idle loop.
// Cheap on the repeat path: one hash lookup plus one page-table probe per load.
bool in_idle_loop(CpuContext& cpu, IdlePorts ports = IdlePorts::All);

// Guest code or a memory mapping changed: drop the structural cache.
void invalidate_idle_loops();

// Diagnostics (DS_PROFILE): how the analysis is landing.
enum class IdleReject : u8 { None, Thumb, Fetch, NoBackEdge, TooLong, Store, LoadForm, BadInstr, PcWrite, Carried, NoLoad, Mmio };
const char* idle_reject_name(IdleReject r);
// Reason the most recent in_idle_loop() query said no (diagnostics only).
IdleReject idle_loop_last_reject();

struct IdleLoopStats {
  u64 queries, hits, analyses;
  u64 by_reason[12];   // indexed by IdleReject
};
const IdleLoopStats& idle_loop_stats();

}  // namespace ds::cpu
