// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) backend: the host register convention shared by the stubs and
// the translator. Nothing outside a32/ sees this.
//
// Thirteen usable registers against the AArch64 backend's thirty-one, so the
// static map that pins every guest register does not transfer
// (docs/arm32-jit-scoping.md). What is pinned:
//
//   r0-r3, r12       scratch (also C call arguments; r12 = helper address)
//   r4-r8            free callee-saved: the register cache's long-lived slots
//                    (phase 2; nothing lives here yet)
//   r9               cycle budget minus one (bit 31 set => leave)
//   r10              page-table base for this CPU
//   r11              CpuContext*
//   r13              sp
//   r14              lr: stubs called with `bl` read their literal arguments
//                    (instruction, key, ...) through it
//
// Phase 1 (this): every guest register and the CPSR live in memory
// (JitHot); each instruction runs through the fallback stub. The host NZCV
// are therefore free, and the stubs and the budget test use them.
#pragma once
#include "core/cpu/jit/jit_internal.h"

namespace ds::jit {

constexpr u32 R_BUDGET = 9, R_PT = 10, R_CTX = 11, R_FN = 12, R_SP = 13, R_LR = 14, R_PC = 15;
constexpr u32 SCRATCH0 = 0, SCRATCH1 = 1, SCRATCH2 = 2, SCRATCH3 = 3;

} // namespace ds::jit
