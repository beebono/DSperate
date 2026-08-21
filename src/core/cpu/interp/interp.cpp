// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/interp/interp.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/nds.h"

#include <algorithm>

namespace ds::interp {

// Cycle model (mirrors melonDS's, which is the reference our traces are
// compared against):
//  - every instruction pays a code-fetch cost numC: on the ARM9 the cost of the
//    prefetch two instructions ahead (0 for the odd halfword of a Thumb pair);
//    on the ARM7 a sequential fetch (S), or a non-sequential one (N) when the
//    instruction had an internal cycle or a data access;
//  - CI adds `internal` cycles; CD/CDI combine numC with the data cost numD,
//    overlapping where the hardware does.
static inline u32 instruction_cycles(CpuContext& cpu, u32 internal, bool jumped) {
  const bool thumb = cpu.thumb();
  const u32 numD = cpu.data_cycles;
  if (cpu.which == Cpu::ARM9) {
    const u32 numC = (thumb && (cpu.hot.regs[15] & 2)) ? 0 : cpu.code_cycles;
    switch (cpu.cycle_class) {
    case CYC_C:   return jumped ? 0 : numC;
    case CYC_CI:  return numC + internal;
    default:      return std::max<s32>(static_cast<s32>(numC + numD) - 6, static_cast<s32>(std::max(numC, numD)));
    }
  }
  const u8* t = cpu.timing7[cpu.code_cycles];
  switch (cpu.cycle_class) {
  case CYC_C:   return jumped ? 0 : t[thumb ? 1 : 3];
  case CYC_CI:  return t[thumb ? 0 : 2] + internal;
  case CYC_CD: {
    const u32 numC = t[thumb ? 0 : 2];
    if (cpu.data_region == 0x02) {
      if (cpu.code_region == 0x02) return numC + numD;
      return std::max<s32>(static_cast<s32>(numC + numD) - 3, static_cast<s32>(std::max(numC, numD)));
    }
    if (cpu.code_region == 0x02) return std::max<s32>(static_cast<s32>(numC + numD) - 3, static_cast<s32>(std::max(numC, numD)));
    return numC + numD;
  }
  default: {  // CDI
    u32 numC = t[thumb ? 0 : 2];
    u32 d = numD;
    if (cpu.data_region == 0x02) {
      if (cpu.code_region == 0x02) return numC + d;
      numC++;
      return std::max<s32>(static_cast<s32>(numC + d) - 3, static_cast<s32>(std::max(numC, d)));
    }
    if (cpu.code_region == 0x02) {
      d++;
      return std::max<s32>(static_cast<s32>(numC + d) - 3, static_cast<s32>(std::max(numC, d)));
    }
    return numC + d + 1;
  }
  }
}

static inline void prefetch_cost9(CpuContext& cpu) {
  const u32 pc = cpu.hot.regs[15];           // two instructions ahead
  if (cpu.thumb() && (pc & 2)) { cpu.code_cycles = 0; return; }
  if (pc < cpu.itcm_size) { cpu.code_cycles = 1; return; }
  const u8 c = cpu.timing9[pc >> 12][0];
  cpu.code_cycles = (c == 0xFF) ? (!(pc & 0x1F) ? 3 : 1) : c;
}

void run(CpuContext& cpu) {
  cpu.check_irq();
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  const bool a9 = cpu.which == Cpu::ARM9;

  while (cpu.hot.cycle_budget > 0) {
    u32 internal;
    cpu.cycle_class = CYC_C;
    cpu.data_cycles = 0;
    cpu.jumped = false;
    if (cpu.thumb()) {
      const u32 pc = cpu.hot.regs[15] - 4;
      const u16 instr = fetch16(cpu, pc);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      internal = exec_thumb(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 2;
    } else {
      const u32 pc = cpu.hot.regs[15] - 8;
      const u32 instr = fetch32(cpu, pc);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      internal = exec_arm(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 4;
    }
    cpu.hot.cycle_budget -= static_cast<s32>(instruction_cycles(cpu, internal, cpu.jumped));
    if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
    if (cpu.hot.irq_pending) cpu.check_irq();
    if (cpu.step_limit && ++cpu.steps >= cpu.step_limit) return;
  }
}

} // namespace ds::interp
