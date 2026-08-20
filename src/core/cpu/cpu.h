// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

namespace ds {

struct NDS;

// Per-CPU state, shared by the interpreter and (on AArch64) the recompiler.
//
// Layout is a contract with the JIT (docs/ARCHITECTURE.md §3), so the struct is
// `standard-layout`, offsets are static_asserted, and fields the JIT touches are
// grouped into `JitHot` so they sit within one signed-9-bit / scaled-12-bit
// immediate range of a single base register.
//
// The page table is a *separate* 16 MiB allocation pointed to by `page_table`;
// the JIT keeps that pointer in a register and places the register spill area
// at a fixed negative offset from it via `spill_anchor` (a pointer into this
// struct), so one base register serves both. Whether to inline the table is an
// open item - see ARCHITECTURE.md.

enum class Mode : u8 {
  USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1B, SYS = 0x1F
};

struct JitHot {
  u32 regs[16];        // r0-r15 home locations; r15 is only valid at sync points
  u32 cpsr;
  u32 spsr;            // SPSR of the current mode (banked copies live below)
  s32 cycle_budget;    // counts DOWN; sign bit => leave translated code
  u32 irq_pending;     // non-zero => an IRQ is waiting to be taken
  u32 alerts;          // scheduler/DMA/SMC "look at me" word polled after stores
  u32 _pad;
  u64 exit_native;     // native resume/exit address
  u64 other_cpu;       // sibling CpuContext*
};

struct CpuContext {
  Cpu  which;
  bool halted;
  u8   _pad0[6];

  JitHot hot;

  // Banked registers, indexed by bank {USR/SYS, FIQ, IRQ, SVC, ABT, UND}.
  u32 bank_r8_r12[2][5];   // USR and FIQ only
  u32 bank_r13[6];
  u32 bank_r14[6];
  u32 bank_spsr[6];

  // CP15 (ARM9 only).
  u32 cp15_control;
  u32 cp15_dtcm;           // DTCM base/size register value
  u32 cp15_itcm;           // ITCM size register value

  mem::PageTable page_table;

  NDS* nds;

  void reset(Cpu which, NDS* nds);
};

static_assert(offsetof(JitHot, cycle_budget) == 72, "JitHot layout changed; update the JIT");
static_assert(sizeof(JitHot) == 104, "JitHot layout changed; update the JIT");

// Pluggable execution engine: interpreter always; JIT on AArch64.
using RunFn = void (*)(CpuContext& cpu);

} // namespace ds
