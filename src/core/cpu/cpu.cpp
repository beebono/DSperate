// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/cpu.h"

#include <cstring>

namespace ds {

void CpuContext::reset(Cpu w, NDS* n) {
  which  = w;
  nds    = n;
  halted = false;
  std::memset(&hot, 0, sizeof hot);
  std::memset(bank_r8_r12, 0, sizeof bank_r8_r12);
  std::memset(bank_r13, 0, sizeof bank_r13);
  std::memset(bank_r14, 0, sizeof bank_r14);
  std::memset(bank_spsr, 0, sizeof bank_spsr);
  cp15_control = cp15_dtcm = cp15_itcm = 0;
  hot.cpsr = static_cast<u32>(Mode::SVC) | 0xC0;   // IRQ+FIQ masked, ARM state
  hot.regs[15] = (w == Cpu::ARM9) ? 0xFFFF0000u : 0x00000000u;  // BIOS entry
}

} // namespace ds
