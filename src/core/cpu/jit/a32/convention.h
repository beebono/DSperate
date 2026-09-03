// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) backend: the host register convention shared by the stubs and
// the translator. Nothing outside a32/ sees this.
//
// Thirteen usable registers against the AArch64 backend's thirty-one, so the
// static map that pins every guest register does not transfer
// (docs/arm32-jit-scoping.md). The phase-0 census (§6 there) put most of the
// register traffic on guest r0-r3 and sp, so those five are pinned in the
// five free callee-saved registers and every block and stub agrees on them;
// the other guest registers live in memory (JitHot::regs) and a per-block
// cache (translate.cpp, RegCache) holds them in the scratch registers while
// a block runs.
//
//   r0-r3, r12       scratch: the cache's slots, the shifter/cost temporaries,
//                    C call arguments (r12 = helper address in the stubs)
//   r4-r7            guest r0-r3     (callee-saved: survive C calls)
//   r8               guest r13
//   r9               cycle budget minus one (bit 31 set => leave)
//   r10              page-table base for this CPU
//   r11              CpuContext*
//   r13              sp
//   r14              lr: stubs called with `bl` read their literal arguments
//                    (instruction, key, ...) through it
//
// Guest NZCV and Q live in the host APSR (bits 31:27); the rest of CPSR lives
// in memory. Q rides along because every APSR write masks nzcvq: the v5TE
// saturating instructions are then native and their sticky flag needs no
// merge of its own.
#pragma once
#include "core/cpu/jit/jit_internal.h"

namespace ds::jit {

constexpr u32 R_BUDGET = 9, R_PT = 10, R_CTX = 11, R_FN = 12, R_SP = 13, R_LR = 14, R_PC = 15;
constexpr u32 SCRATCH0 = 0, SCRATCH1 = 1, SCRATCH2 = 2, SCRATCH3 = 3, SCRATCH4 = 12;

// Host register of a pinned guest register, or 0xFF.
inline constexpr u32 pinned_host(u32 guest) {
  return guest < 4 ? 4 + guest : guest == 13 ? 8 : 0xFFu;
}
constexpr u32 PINNED_MASK = 0xFu | (1u << 13);

} // namespace ds::jit
