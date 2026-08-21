// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/interp/interp.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/cpu/cpu_cycles.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>


namespace ds::interp {

void run(CpuContext& cpu) {
  cpu.check_irq();
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  const bool a9 = cpu.which == Cpu::ARM9;

  // Each handler charges its own cycles (cpu_cycles.h) at the point melonDS
  // does; the loop only computes the ARM9 prefetch cost and advances r15.
  while (cpu.hot.cycle_budget > 0) {
    cpu.data_cycles = 0;
    cpu.jumped = false;
    // DS_DEBUG_CYCLES=1: budget before every instruction (engine lockstep debugging).
    static const bool debug_cycles = std::getenv("DS_DEBUG_CYCLES") != nullptr;
    if (debug_cycles) std::fprintf(stderr, "[cyc%d] %08x %d\n", a9 ? 9 : 7, cpu.hot.regs[15], cpu.hot.cycle_budget);
    if (cpu.thumb()) {
      const u32 pc = cpu.hot.regs[15] - 4;
      const u16 instr = fetch16(cpu, pc);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      exec_thumb(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 2;
    } else {
      const u32 pc = cpu.hot.regs[15] - 8;
      const u32 instr = fetch32(cpu, pc);
      if (cpu.nds->trace) cpu.nds->trace(cpu, instr, cpu.nds->trace_user);
      if (a9) prefetch_cost9(cpu);
      exec_arm(cpu, instr);
      if (!cpu.jumped) cpu.hot.regs[15] += 4;
    }
    if (cpu.halted) { cpu.budget_at_halt = cpu.hot.cycle_budget; cpu.hot.cycle_budget = -1; return; }
    if (cpu.hot.irq_pending) cpu.check_irq();
    if (cpu.step_limit && ++cpu.steps >= cpu.step_limit) return;
  }
}

} // namespace ds::interp
