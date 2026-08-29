// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

namespace ds {

struct NDS;

// Per-CPU state, shared by the interpreter and (on AArch64) the recompiler.
//
// Layout is a contract with the JIT, so the struct is `standard-layout`,
// offsets are static_asserted, and fields the JIT touches are
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
  s32  preempt_residual = 0;  // budget handed back when a DMA started by this CPU cuts its slice short (see Scheduler)
  bool jumped;       // set by jump(); cleared by the interpreter before each instruction
  u8   _pad0[5];

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
  u32 pu_region[8];        // c6,cN,0: base/size/enable
  u32 pu_code_cacheable;   // c2,c0,1 (bit per region)
  u32 pu_data_cacheable;   // c2,c0,0
  u32 pu_data_bufferable;  // c3,c0,0
  u32 pu_code_perm, pu_data_perm;   // c5,c0,{3,2}
  u32 itcm_size;           // effective ITCM window (bytes), 0 when disabled
  u32 dtcm_base, dtcm_mask;// DTCM window: (addr & dtcm_mask) == dtcm_base

  // ---- cycle accounting ----
  // Per-4 KB timing for the ARM9: [0] code cost in ARM9 cycles or 0xFF when
  // the page is instruction-cacheable; [1] load N16, [2] load N32, [3] load
  // S32, [5..7] the same for stores (ARM9 cycles; [4] unused). Per-32 KB for
  // the ARM7: N16, S16, N32, S32.
  const u8 (*timing9)[8];
  const u8 (*timing7)[4];
  // ARM7 precomputed data cost (mem::Timing::cost7), used by the recompiler's
  // single-access path; it reaches this from timing7 with one add.
  const u8* cost7;
  u32 code_cycles;         // ARM9: cost of the most recent prefetch. ARM7: code-region table index.
  u32 data_cycles;         // accumulated data-access cost of the current instruction
  u32 code_region, data_region;   // high byte of the address (ARM7 main-RAM overlap rules)
  u8  _pad1;
  bool branch_fetch;       // ARM9: the next prefetch is the first after a branch

  // Debug single-stepping: when step_limit != 0 the interpreter stops after
  // that many instructions regardless of the cycle budget.
  u32 step_limit, steps;
  // Budget at the moment the CPU halted (the run loops then set the budget
  // to -1 to end the slice). Lets a harness compare consumed cycles across
  // engines when a trial ends in a halt.
  s32 budget_at_halt;

  mem::PageTable page_table;

  NDS* nds;

  // Recompiler hooks (null when the CPU runs on the interpreter). The timing
  // callback fires whenever the per-page cost table or the TCM windows change,
  // since translated code bakes those costs in.
  void (*jit_timing_changed)(CpuContext&) = nullptr;
  void* jit = nullptr;

  void reset(Cpu which, NDS* nds);

  // Mode handling. `switch_mode` banks r8-r14 and SPSR as needed.
  void switch_mode(u32 new_mode);
  void set_cpsr(u32 value);          // full write incl. mode switch
  void restore_cpsr();               // CPSR <- SPSR (exception return)

  // Exceptions. `return_offset` is added to the current r15 to form LR.
  enum class Exception : u8 { Reset, Undefined, Swi, PrefetchAbort, DataAbort, Irq, Fiq };
  void raise_exception(Exception e);
  void update_tcm_windows();         // recompute itcm_size/dtcm_base/dtcm_mask from CP15
  void check_irq();                  // take a pending IRQ if unmasked

  // Control flow. `addr` bit 0 selects Thumb when `interwork`.
  void jump(u32 addr, bool interwork);
  bool thumb() const { return hot.cpsr & 0x20; }
  u32  exception_base() const;
};

static_assert(offsetof(JitHot, cycle_budget) == 72, "JitHot layout changed; update the JIT");
static_assert(sizeof(JitHot) == 104, "JitHot layout changed; update the JIT");

// Pluggable execution engine: interpreter always; JIT on AArch64.
using RunFn = void (*)(CpuContext& cpu);

} // namespace ds
