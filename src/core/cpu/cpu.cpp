// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/cpu.h"
#include "core/cpu/cpu_cycles.h"

#include <cstring>

namespace ds {

namespace {
// Bank index for each mode: {USR/SYS, FIQ, IRQ, SVC, ABT, UND}.
int bank_of(u32 mode) {
  switch (mode & 0x1F) {
  case 0x11: return 1;
  case 0x12: return 2;
  case 0x13: return 3;
  case 0x17: return 4;
  case 0x1B: return 5;
  default:   return 0;   // USR, SYS
  }
}
} // namespace

void CpuContext::reset(Cpu w, NDS* n) {
  which  = w;
  nds    = n;
  halted = false;
  jumped = false;
  std::memset(&hot, 0, sizeof hot);
  std::memset(bank_r8_r12, 0, sizeof bank_r8_r12);
  std::memset(bank_r13, 0, sizeof bank_r13);
  std::memset(bank_r14, 0, sizeof bank_r14);
  std::memset(bank_spsr, 0, sizeof bank_spsr);
  cp15_control = (w == Cpu::ARM9) ? 0x00002078u : 0;   // V=0 until the BIOS sets it; TCM disabled
  cp15_dtcm = cp15_itcm = 0;
  std::memset(pu_region, 0, sizeof pu_region);
  pu_code_cacheable = pu_data_cacheable = pu_data_bufferable = 0;
  pu_code_perm = pu_data_perm = 0;
  itcm_size = 0; dtcm_base = 0xFFFFFFFF; dtcm_mask = 0;
  code_cycles = data_cycles = 0; code_region = data_region = 0; branch_fetch = true;
  step_limit = steps = 0;
  hot.cpsr = static_cast<u32>(Mode::SVC) | 0xC0;       // IRQ+FIQ masked, ARM state
  hot.regs[15] = exception_base() + 8;                 // reset vector, pipeline-adjusted
}

u32 CpuContext::exception_base() const {
  if (which == Cpu::ARM9) return (cp15_control & (1u << 13)) ? 0xFFFF0000u : 0x00000000u;
  return 0x00000000u;
}

void CpuContext::switch_mode(u32 new_mode) {
  const u32 old_mode = hot.cpsr & 0x1F;
  new_mode &= 0x1F;
  if (old_mode == new_mode) return;
  const int ob = bank_of(old_mode), nb = bank_of(new_mode);
  if (ob == nb) { hot.cpsr = (hot.cpsr & ~0x1Fu) | new_mode; return; }

  // Save outgoing bank.
  bank_r13[ob] = hot.regs[13];
  bank_r14[ob] = hot.regs[14];
  if (ob != 0) bank_spsr[ob] = hot.spsr;
  if (ob == 1) std::memcpy(bank_r8_r12[1], &hot.regs[8], 5 * sizeof(u32));
  else if (nb == 1) std::memcpy(bank_r8_r12[0], &hot.regs[8], 5 * sizeof(u32));

  // Load incoming bank.
  hot.regs[13] = bank_r13[nb];
  hot.regs[14] = bank_r14[nb];
  hot.spsr     = (nb != 0) ? bank_spsr[nb] : 0;
  if (nb == 1) std::memcpy(&hot.regs[8], bank_r8_r12[1], 5 * sizeof(u32));
  else if (ob == 1) std::memcpy(&hot.regs[8], bank_r8_r12[0], 5 * sizeof(u32));

  hot.cpsr = (hot.cpsr & ~0x1Fu) | new_mode;
}

void CpuContext::set_cpsr(u32 value) {
  switch_mode(value & 0x1F);
  hot.cpsr = value;
}

void CpuContext::restore_cpsr() {
  const u32 spsr = hot.spsr;
  set_cpsr(spsr);
}

void CpuContext::update_tcm_windows() {
  if (which != Cpu::ARM9) return;
  if (cp15_control & (1u << 18)) {
    itcm_size = 512u << ((cp15_itcm >> 1) & 0x1F);
    if (itcm_size < 0x8000) itcm_size = 0x8000;
  } else itcm_size = 0;
  if (cp15_control & (1u << 16)) {
    u32 size = 512u << ((cp15_dtcm >> 1) & 0x1F);
    if (size < 0x4000) size = 0x4000;
    dtcm_mask = ~(size - 1);
    dtcm_base = cp15_dtcm & dtcm_mask;
  } else { dtcm_base = 0xFFFFFFFF; dtcm_mask = 0; }
}

void CpuContext::jump(u32 addr, bool interwork) {
  jumped = true;
  if (interwork) {
    if (addr & 1) hot.cpsr |= 0x20; else hot.cpsr &= ~0x20u;
  }
  if (thumb()) hot.regs[15] = (addr & ~1u) + 4;
  else         hot.regs[15] = (addr & ~3u) + 8;

  // Pipeline refill cost (the recompiler emits the same arithmetic). On the
  // ARM9 the refill leaves the cost of its last fetch in code_cycles; on the
  // ARM7 the code region follows the target.
  u32 code_after = 0;
  hot.cycle_budget -= static_cast<s32>(refill_cycles(*this, addr & ~1u, thumb(), &code_after));
  if (which == Cpu::ARM9) { code_cycles = code_after; branch_fetch = false; }
  else { code_cycles = (addr & ~1u) >> 15; code_region = addr >> 24; }
}

void CpuContext::raise_exception(Exception e) {
  u32 vector = 0, mode = 0x13; s32 lr_adj = 0;
  const bool was_thumb = thumb();
  switch (e) {
  case Exception::Reset:         vector = 0x00; mode = 0x13; break;
  case Exception::Undefined:     vector = 0x04; mode = 0x1B; lr_adj = was_thumb ? 2 : 4; break;
  case Exception::Swi:           vector = 0x08; mode = 0x13; lr_adj = was_thumb ? 2 : 4; break;
  case Exception::PrefetchAbort: vector = 0x0C; mode = 0x17; lr_adj = was_thumb ? 0 : 4; break;
  case Exception::DataAbort:     vector = 0x10; mode = 0x17; lr_adj = was_thumb ? -4 : 0; break;
  case Exception::Irq:           vector = 0x18; mode = 0x12; lr_adj = was_thumb ? 0 : 4; break;
  case Exception::Fiq:           vector = 0x1C; mode = 0x11; lr_adj = was_thumb ? 0 : 4; break;
  }
  // LR per exception, in terms of r15 at the moment the exception is raised:
  //  - SWI/UND/aborts are raised *during* execution, when r15 = addr + 8 (ARM)
  //    or addr + 4 (Thumb);
  //  - IRQ/FIQ are raised *between* instructions, when r15 = next + 8 / next + 4,
  //    and the architectural LR is next + 4 in both states.
  const u32 pc = hot.regs[15];
  const u32 old_cpsr = hot.cpsr;
  switch_mode(mode);
  hot.spsr = old_cpsr;
  hot.regs[14] = static_cast<u32>(static_cast<s32>(pc) - lr_adj);
  hot.cpsr &= ~0x20u;                       // ARM state
  hot.cpsr |= 0x80;                         // mask IRQ
  // The architecture leaves F alone on IRQ entry; melonDS (our trace oracle)
  // sets it, and FIQ is unused on the DS, so follow melonDS for lockstep.
  if (e == Exception::Fiq || e == Exception::Reset || e == Exception::Irq) hot.cpsr |= 0x40;
  jump(exception_base() + vector, false);   // charges the pipeline refill like any branch
}

void CpuContext::check_irq() {
  if (hot.irq_pending && !(hot.cpsr & 0x80)) {
    halted = false;
    raise_exception(Exception::Irq);
  }
}

} // namespace ds
