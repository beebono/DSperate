// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Recompiler internals shared by the runtime (stubs, cache) and the
// translator. Nothing here is visible outside cpu/jit.
#pragma once
#include "core/cpu/jit/jit.h"
#include "core/cpu/jit/emit.h"
#include "core/cpu/cpu.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace ds::jit {

// ---- host register convention -------------------------------------------------
// Translated code runs with the guest registers pinned; every block and every
// stub agrees on this map, so linked blocks reconcile nothing at the edge.
//
//   x0-x7, x16, x17   scratch (also C call arguments)
//   w8                cycle budget (counts down; bit 31 set => leave)
//   x9-x13            guest r8-r12   (caller-saved: the call stubs spill them)
//   x14               page-table base for this CPU
//   x15               per-page timing table (timing9 or timing7)
//   x19-x26           guest r0-r7    (callee-saved: survive C calls)
//   x27, x28          guest r13, r14
//   x29               CpuContext*
//   x30               link register (only live inside call sequences)
//
// Guest NZCV live in the host NZCV; the rest of CPSR lives in memory.
constexpr u32 R_BUDGET = 8, R_PT = 14, R_TIM = 15, R_CTX = 29, R_LR = 30;
constexpr u32 SCRATCH0 = 0, SCRATCH1 = 1, SCRATCH2 = 2, SCRATCH3 = 3, SCRATCH4 = 4, SCRATCH5 = 5, SCRATCH6 = 6, SCRATCH7 = 7;
constexpr u32 R_FN = 16;        // function address for the call stubs

inline constexpr u32 host_reg(u32 guest) {
  return guest < 8 ? 19 + guest : guest < 13 ? 9 + (guest - 8) : guest == 13 ? 27 : 28;
}
inline constexpr bool host_reg_callee_saved(u32 guest) { return guest < 8 || guest >= 13; }

// ---- CpuContext offsets ---------------------------------------------------------
constexpr u32 OFF_HALTED   = offsetof(CpuContext, halted);
constexpr u32 OFF_JUMPED   = offsetof(CpuContext, jumped);
constexpr u32 OFF_REGS     = offsetof(CpuContext, hot) + offsetof(JitHot, regs);
constexpr u32 OFF_CPSR     = offsetof(CpuContext, hot) + offsetof(JitHot, cpsr);
constexpr u32 OFF_BUDGET   = offsetof(CpuContext, hot) + offsetof(JitHot, cycle_budget);
constexpr u32 OFF_IRQ      = offsetof(CpuContext, hot) + offsetof(JitHot, irq_pending);
constexpr u32 OFF_ALERTS   = offsetof(CpuContext, hot) + offsetof(JitHot, alerts);
constexpr u32 OFF_JIT      = offsetof(CpuContext, jit);
constexpr u32 OFF_JC_PT    = 0;    // JitCpuHot::pt
constexpr u32 OFF_JC_TIM   = 8;    // JitCpuHot::timing
inline constexpr u32 off_reg(u32 r) { return OFF_REGS + 4 * r; }

// Alert bits (JitHot::alerts): set by the runtime while translated code is
// inside a helper; the post-helper poll leaves the block when any is set.
constexpr u32 ALERT_INVALIDATED = 1;   // a block was invalidated (possibly this one)

// ---- blocks -----------------------------------------------------------------------
// Key = guest address with bit 0 = Thumb. ARM keys are word aligned, Thumb
// keys halfword aligned, so the key is unique per (pc, state).
inline constexpr u32 make_key(u32 pc, bool thumb) { return thumb ? (pc & ~1u) | 1u : (pc & ~3u); }
inline constexpr u32 key_pc(u32 key) { return key & ~1u; }
inline constexpr bool key_thumb(u32 key) { return key & 1; }
inline constexpr u32 key_r15(u32 key) { return key_pc(key) + (key_thumb(key) ? 4 : 8); }

struct Block {
  u32  key;
  u8*  entry;
  u32  size;         // bytes of native code
  u32  guest_len;    // bytes of guest code covered
  const u8* host_pages[2];   // 2 KB host pages the guest code lives in (0-2 used)
  u32  npages;
  u8   owner;        // index into Runtime::cpus
  bool dead;
};

constexpr u32 LUT_BITS = 16;
constexpr u32 LUT_SIZE = 1u << LUT_BITS;
constexpr u32 LUT_EMPTY_KEY = 0xFFFFFFFFu;

// Standard-layout head of JitCpu: translated code reaches these through
// CpuContext::jit with fixed offsets.
struct JitCpuHot {
  u64*      pt;        // page-table entries
  const u8* timing;    // timing9 (per 4 KB) or timing7 (per 32 KB), 4 bytes per entry
};

struct JitCpu {
  JitCpuHot hot{};
  CpuContext* ctx = nullptr;
  NDS*  nds = nullptr;
  bool  arm9 = false;
  u64*  lut = nullptr;                       // LUT_SIZE entries: (native offset << 32) | key
  std::unordered_map<u32, Block*> blocks;
  std::vector<Block*> all_blocks;            // for flushes
  u8*   dispatch = nullptr;                  // w0 = key -> jumps to the block
  u8*   link = nullptr;                      // bl'd from a block with w0 = key; patches the bl
  u8*   branch_indirect = nullptr;           // w0 = target (bit 0 = T): updates T, charges refill, dispatches
  u8*   branch_indirect_cdi = nullptr;       // same, plus the post-jump CDI charge of LDM/POP pc: w1 = numD, w2 = data address
};

struct Runtime {
  u8*    arena = nullptr;
  size_t cap = 0;
  size_t pos = 0;
  size_t stubs_end = 0;        // arena below this is permanent
  bool   need_reset = false;   // arena full: reset at the next safe point

  // C-callable: void enter(CpuContext*, const void* native)
  void (*enter)(CpuContext*, const void*) = nullptr;
  u8* exit_key = nullptr;      // w0 = key of the next instruction; stores r15, leaves
  u8* exit_r15 = nullptr;      // ctx.r15 already correct; leaves
  u8* call_pure = nullptr;     // x16 = fn; spills caller-saved guest regs, flags, budget
  u8* call_full = nullptr;     // x16 = fn; spills everything, reloads everything
  u8* flush_exit = nullptr;    // w0 = key; request arena reset and leave

  JitCpu cpus[2];
  std::unordered_map<const u8*, std::vector<Block*>> code_pages;   // host page -> blocks
  bool trace = false;
  bool strict = false;    // check the budget after every instruction (exact lockstep with the interpreter)
  bool debug = false;     // DS_JIT_DEBUG: log fallbacks
  bool cyclog = false;    // DS_DEBUG_CYCLES: log the budget after every instruction (needs strict)
  bool hist = false;      // DS_JIT_HIST: histogram of fallback executions by pc
  bool fastcost = false;  // DS_JIT_FASTCOST: measurement knob (inexact data-cost arithmetic)
  std::unordered_map<u64, u64> fallback_hist;
  Stats stats;
};

Runtime& rt();

// Runtime services used by the translator.
void   invalidate_host_page(const u8* host_page);
void   invalidate_cpu(JitCpu& jc);
void   lut_insert(JitCpu& jc, Block* b);
Block* translate(JitCpu& jc, u32 key);          // null when the arena is full
const u8* find_native(JitCpu& jc, u32 key);      // translates on miss; null when arena is full
// translate.cpp: emit one block for `key` at the emitter's position. Returns
// false when the emitter ran out of room (the caller resets the arena).
bool   translate_block(JitCpu& jc, u32 key, Emitter& e, Block& b);

// Helpers called from translated code (through call_pure / call_full).
extern "C" {
u32         jit_h_fallback(CpuContext* cpu, u32 instr, u32 key);   // returns cpu->jumped
const void* jit_h_lookup(CpuContext* cpu, u32 key);
const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site);
void        jit_h_trace(CpuContext* cpu, u32 instr, u32 key);
void        jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key);
}

// Flush the instruction cache for freshly written code.
inline void sync_icache(u8* start, size_t len) { __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(start + len)); }

} // namespace ds::jit
