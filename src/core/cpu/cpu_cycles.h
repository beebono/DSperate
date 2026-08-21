// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Per-instruction cycle model, shared by the interpreter and the recompiler
// (which emits the same arithmetic inline and calls the interpreter for the
// rest). It mirrors melonDS's, the reference our traces are compared against:
//
//  - every instruction pays a code-fetch cost numC. On the ARM9 this is the
//    cost of the prefetch two instructions ahead (`code_cycles`, computed
//    before the instruction runs, 0 for the odd halfword of a Thumb pair);
//    on the ARM7 it is a sequential fetch (S) for plain instructions and a
//    non-sequential one (N) for instructions with an internal cycle or a data
//    access;
//  - CI adds `internal` cycles; CD/CDI combine numC with the data cost numD,
//    overlapping where the hardware does;
//  - branches pay the pipeline refill instead (`refill_cycles`, charged by
//    CpuContext::jump), and nothing else.
//
// *Where* an instruction charges matters: the handler calls charge_* at the
// point melonDS does, which for instructions that may jump is either before
// the jump (ALU ops, LDR, Thumb hi-register ops: the old pc and state) or
// after it (LDM, POP: the new pc, state and code region).
#pragma once
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_mem.h"

#include <algorithm>

namespace ds {

// ARM9 numC at the moment of charging: the odd halfword of a Thumb pair is
// already in the prefetch buffer.
inline u32 num_c9(const CpuContext& cpu) {
  return (cpu.thumb() && (cpu.hot.regs[15] & 2)) ? 0 : cpu.code_cycles;
}

inline void charge_C(CpuContext& cpu) {
  if (cpu.which == Cpu::ARM9) { cpu.hot.cycle_budget -= static_cast<s32>(num_c9(cpu)); return; }
  cpu.hot.cycle_budget -= cpu.timing7[cpu.code_cycles][cpu.thumb() ? 1 : 3];
}

inline void charge_CI(CpuContext& cpu, u32 internal) {
  if (cpu.which == Cpu::ARM9) { cpu.hot.cycle_budget -= static_cast<s32>(num_c9(cpu) + internal); return; }
  cpu.hot.cycle_budget -= static_cast<s32>(cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2] + internal);
}

inline u32 max3(s32 a, s32 b, s32 c) { return static_cast<u32>(std::max(a, std::max(b, c))); }

// Loads (the ARM9 skips the internal cycle; the ARM7 pays it unless main RAM absorbs it).
inline void charge_CDI(CpuContext& cpu) {
  const s32 numD = static_cast<s32>(cpu.data_cycles);
  if (cpu.which == Cpu::ARM9) {
    const s32 numC = static_cast<s32>(num_c9(cpu));
    cpu.hot.cycle_budget -= static_cast<s32>(max3(numC + numD - 6, numC, numD));
    return;
  }
  s32 numC = cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2];
  s32 d = numD;
  u32 cost;
  if (cpu.data_region == 0x02) {
    if (cpu.code_region == 0x02) cost = static_cast<u32>(numC + d);
    else { numC++; cost = max3(numC + d - 3, numC, d); }
  } else {
    if (cpu.code_region == 0x02) { d++; cost = max3(numC + d - 3, numC, d); }
    else cost = static_cast<u32>(numC + d + 1);
  }
  cpu.hot.cycle_budget -= static_cast<s32>(cost);
}

// Stores.
inline void charge_CD(CpuContext& cpu) {
  const s32 numD = static_cast<s32>(cpu.data_cycles);
  if (cpu.which == Cpu::ARM9) {
    const s32 numC = static_cast<s32>(num_c9(cpu));
    cpu.hot.cycle_budget -= static_cast<s32>(max3(numC + numD - 6, numC, numD));
    return;
  }
  const s32 numC = cpu.timing7[cpu.code_cycles][cpu.thumb() ? 0 : 2];
  u32 cost;
  if ((cpu.data_region == 0x02) == (cpu.code_region == 0x02)) cost = static_cast<u32>(numC + numD);
  else cost = max3(numC + numD - 3, numC, numD);
  cpu.hot.cycle_budget -= static_cast<s32>(cost);
}

// ARM9 prefetch cost for the instruction about to execute (r15 = two ahead).
inline void prefetch_cost9(CpuContext& cpu) {
  const u32 pc = cpu.hot.regs[15];
  if (cpu.thumb() && (pc & 2)) { cpu.code_cycles = 0; return; }
  const u8 c = cpu.timing9[pc >> 12][0];
  cpu.code_cycles = (c == 0xFF) ? (!(pc & 0x1F) ? 3 : 1) : c;
}

// Cost of one 32-bit instruction fetch at `addr`, in ARM9 cycles. `branch`
// marks the first fetch after a branch (a cache line fill on cacheable code).
inline u32 fetch_cost9(const CpuContext& cpu, u32 addr, bool branch) {
  const u8 c = cpu.timing9[addr >> 12][0];
  if (c == 0xFF) return (branch || !(addr & 0x1F)) ? 3 : 1;   // cache line fill vs hit (approximation)
  return c;
}

// Pipeline refill charged by a jump to `addr` (bit 0 clear) in the given
// state. `code_after` receives the ARM9 prefetch cost left in `code_cycles`
// by the refill (the cost of the last fetch it performed), which a
// post-jump charge (LDM pc, POP pc) uses as numC.
inline u32 refill_cycles(const CpuContext& cpu, u32 addr, bool thumb, u32* code_after = nullptr) {
  if (cpu.which == Cpu::ARM9) {
    u32 c, last;
    if (thumb) {
      if (addr & 2) { last = fetch_cost9(cpu, addr + 2, false); c = fetch_cost9(cpu, addr - 2, true) + last; }
      else { last = fetch_cost9(cpu, addr, true); c = last; }
    } else {
      last = fetch_cost9(cpu, addr + 4, false);
      c = fetch_cost9(cpu, addr, true) + last;
    }
    if (code_after) *code_after = last;
    return c;
  }
  const u8* t = cpu.timing7[addr >> 15];
  return thumb ? (t[0] + t[1]) : (t[2] + t[3]);
}

} // namespace ds
