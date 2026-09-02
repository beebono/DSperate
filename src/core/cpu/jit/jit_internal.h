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
#include <deque>
#include <unordered_map>
#include <vector>

namespace ds::jit {

// ---- host register convention -------------------------------------------------
// Translated code runs with the guest registers pinned; every block and every
// stub agrees on this map, so linked blocks reconcile nothing at the edge.
//
//   x0-x7, x16, x17   scratch (also C call arguments)
//   w8                cycle budget minus one (bit 31 set => leave): one `tbnz`
//                     tests it; the stubs add/subtract the one at the boundary
//   x9-x13            guest r8-r12   (caller-saved: the call stubs spill them)
//   x14               page-table base for this CPU
//   x15               per-page timing table (timing9 or timing7)
//   x18               base of the code arena: the branch LUTs sit at its front
//                     and every block pointer is a 32-bit offset from it, so
//                     one register serves the dispatch probe and the jump.
//                     Linux leaves the platform register alone; C code may
//                     clobber it, so the call stubs reload it like x14/x15.
//   x19-x26           guest r0-r7    (callee-saved: survive C calls)
//   x27, x28          guest r13, r14
//   x29               CpuContext*
//   x30               link register: stubs called with `bl` read their literal
//                     arguments (instruction, key, ...) through it
//
// Guest NZCV live in the host NZCV; the rest of CPSR lives in memory.
constexpr u32 R_BUDGET = 8, R_PT = 14, R_TIM = 15, R_ARENA = 18, R_CTX = 29, R_LR = 30;
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
constexpr u32 OFF_JC_ARENA = 16;   // JitCpuHot::arena
inline constexpr u32 off_reg(u32 r) { return OFF_REGS + 4 * r; }

// Alert bits (JitHot::alerts): set by the runtime while translated code is
// inside a helper; the post-helper poll leaves the block when any is set.
constexpr u32 ALERT_INVALIDATED = 1;   // a block was invalidated (possibly this one)
constexpr u32 ALERT_HALTED      = 2;   // the CPU halted inside a helper

// ---- blocks -----------------------------------------------------------------------
// Key = guest address with bit 0 = Thumb. ARM keys are word aligned, Thumb
// keys halfword aligned, so the key is unique per (pc, state).
inline constexpr u32 make_key(u32 pc, bool thumb) { return thumb ? (pc & ~1u) | 1u : (pc & ~3u); }
inline constexpr u32 key_pc(u32 key) { return key & ~1u; }
inline constexpr bool key_thumb(u32 key) { return key & 1; }
inline constexpr u32 key_r15(u32 key) { return key_pc(key) + (key_thumb(key) ? 4 : 8); }
inline constexpr u32 key_next(u32 key) { return key + (key_thumb(key) ? 2 : 4); }

// DS_JIT_DENSITY: one slot per *translation*, so a block that is retranslated
// after an invalidation is a fresh slot and its executions are not merged with
// the old one's. `execs` is bumped by the block's own entry code; the other two
// are static properties of that translation. Weighting them by `execs` turns
// the emitted-bytes-per-guest-instruction figure into an executed one -- the
// static figure counts a block translated once and run a million times exactly
// as it counts one translated once and run once.
struct DensitySlot {
  u64 execs = 0;          // block entries (bumped from translated code)
  u32 hot_bytes = 0;      // hot section, the instrumentation itself excluded
  u32 guest_instrs = 0;   // guest instructions translated inline into this block
};

constexpr u32 GUEST_COPY_MAX = 64 * 4;   // a block is at most 64 ARM instructions

struct Block {
  u32  key;
  u8*  entry;
  u32  size;         // bytes of native code
  u32  hot_size;     // of which the hot section (the cold section follows it)
  u32  guest_len;    // bytes of guest code covered
  const u8* host_pages[2];   // 2 KB host pages the guest code lives in (0-2 used)
  u32  npages;
  const u8* host_lo;         // first and last host byte of the guest code: a store that
  const u8* host_hi;         // touches neither page's part of [lo, hi] leaves the block alone
  u8   owner;        // index into Runtime::cpus
  bool dead;
  bool pooled;       // lives in Runtime::block_pool (freed by the arena reset), not the heap
  // Static branch targets (emit_branch_static keys): what the pre-translation
  // worker chases ahead of execution. Best-effort -- targets past `nsucc` 4
  // are simply not chased.
  u32  succ[4];
  u8   nsucc;
  // Park-and-revive (see kill_block / revive in runtime.cpp): a block killed
  // by a store into its range keeps its translation, the guest bytes it was
  // built from, the three entry words the kill overwrites, and the timing
  // stamp it was built under. When the same key is looked up again and the
  // guest bytes match one parked version, that version comes back instead of
  // a retranslation. Exact: the translation is a pure function of (key, guest
  // bytes, timing stamp, CPU), and everything the kill undid is redone.
  u32  entry_words[3];
  u64  stamp;
  u32  guest_copy_len;
  u8   guest_copy[GUEST_COPY_MAX];
  // Timing-table dependencies (ARM9): the 4 KB pages whose entry this
  // translation baked a byte from, with which byte (mem::Timing::RETIME_*).
  // Code pages (the prefetch address of every instruction: up to two), the
  // static branch target's refill pages (up to two) and the literal pages of
  // pc-relative loads. A translation that needs more than fit sets
  // `dep_overflow` and dies on every retime, as every block used to.
  static constexpr u32 DEP_MAX = 8;
  u32  dep_page[DEP_MAX];
  u8   dep_kind[DEP_MAX];
  u8   ndep;
  bool dep_overflow;
};

// Direct-mapped branch-target cache, one per CPU, indexed by `(key >> 1)`.
// Each entry is one u64, `(native offset << 32) | key`, so a probe is a single
// load and a tag compare.
//
// Size: 64 K entries, 512 KB per CPU. This is *not* the technique's sizing and
// the difference was measured, not assumed: on the RK3566, 1024 entries costs
// 1.6-2.7 % of frame time against 64 K, and 8 K entries is the break-even.
// Bigger than 64 K gains nothing. See README.md, "The branch LUT", for the
// numbers and why the technique's footprint argument does not transfer here.
// To re-measure, change LUT_BITS and time the device; the arena reserves room
// for LUT_BITS_MAX either way. Miss rate needs a temporary counter in
// `jit_h_lookup`, which is the dispatch stub's only miss path.
constexpr u32 LUT_BITS = 16;
constexpr u32 LUT_BITS_MAX = 16;
constexpr u32 LUT_SIZE = 1u << LUT_BITS;
constexpr u32 LUT_EMPTY_KEY = 0xFFFFFFFFu;
// Arena layout: [CPU0 LUT][CPU1 LUT][stubs][blocks...]. The reservation is
// fixed at the maximum so the per-CPU offsets are constants in the stubs;
// only the first LUT_SIZE entries of each are ever touched.
constexpr size_t LUT_STRIDE = (size_t{1} << LUT_BITS_MAX) * 8;   // 512 KB
constexpr size_t LUT_AREA   = 2 * LUT_STRIDE;
static_assert(LUT_BITS <= LUT_BITS_MAX, "the arena only reserves room for LUT_BITS_MAX");

// Standard-layout head of JitCpu: translated code reaches these through
// CpuContext::jit with fixed offsets.
struct JitCpuHot {
  u64*      pt;        // page-table entries
  const u8* timing;    // timing9 (per 4 KB, 8 bytes per entry) or timing7 (per 32 KB, 4 bytes per entry)
  u8*       arena;     // Runtime::arena: LUT base and block-pointer base (R_ARENA)
};

struct JitCpu {
  JitCpuHot hot{};
  CpuContext* ctx = nullptr;
  NDS*  nds = nullptr;
  bool  arm9 = false;
  u64*  lut = nullptr;                       // into the arena: LUT_SIZE entries, (native offset << 32) | key
  std::unordered_map<u32, Block*> blocks;
  std::vector<Block*> all_blocks;            // for flushes
  std::unordered_map<u32, std::vector<Block*>> parked;   // key -> killed translations kept for revival (newest last)
  u8*   dispatch = nullptr;                  // w0 = key -> jumps to the block
  u8*   link = nullptr;                      // `bl link; .word key`: patches the bl into `b block`
  u8*   fallback = nullptr;                  // `bl fallback; .word instr; .word key`: interpreter for one instruction, poll, dispatch if it jumped
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
  // Same without the callee-saved frame: for callers that saved x19-x28
  // themselves and keep nothing in them (the native slice loop).
  u8* enter_light = nullptr;
  // void run_loop(Scheduler*): the native slice loop (runtime.cpp, jit::run_loop)
  void (*run_loop)(void*) = nullptr;
  u8* exit_key = nullptr;      // w0 = key of the next instruction; stores r15, leaves
  u8* exit_key_lit = nullptr;  // `bl exit_key_lit; .word key`
  u8* exit_r15 = nullptr;      // ctx.r15 already correct; leaves
  u8* call_pure = nullptr;     // x16 = fn; spills caller-saved guest regs, flags, budget
  u8* call_full = nullptr;     // x16 = fn; spills everything, reloads everything
  u8* call2 = nullptr;         // `bl call2; .word a; .word b; .xword fn`: call_full fn(ctx, a, b)
  u8* poll = nullptr;          // `bl poll; .word next_key`: leave on budget/alert/IRQ, else return
  u8* flush_exit = nullptr;    // w0 = key; request arena reset and leave
  u8* slow_load[3] = {};       // w1 = address -> w0 = value (8/16/32); preserves x1, x7
  u8* slow_store[3] = {};      // w1 = address, w2 = value; preserves x1, x7
  u8* merge_keep_cv = nullptr; // w0 = result: N,Z from it, C,V kept
  u8* merge_set_c = nullptr;   // w0 = result, w1 = carry: N,Z from it, C from w1, V kept

  JitCpu cpus[2];
  // Blocks translated on the emulation thread; same lifetime as the arena
  // (deque: stable addresses). One malloc per block was a measurable slice
  // of an overlay burst's translate stall.
  std::deque<Block> block_pool;
  std::unordered_map<const u8*, std::vector<Block*>> code_pages;   // host page -> blocks
  bool trace = false;
  bool strict = false;    // check the budget after every instruction (exact lockstep with the interpreter)
  bool debug = false;     // DS_JIT_DEBUG: log fallbacks
  bool cyclog = false;    // DS_DEBUG_CYCLES: log the budget after every instruction (needs strict)
  bool density = false;   // DS_JIT_DENSITY: count block entries so bytes-per-guest-instruction
                          // can be weighted by execution instead of by translation.
  // deque: the entry code holds the absolute address of a slot's `execs`, so
  // slots must never move. Only the emulation thread appends (DS_JIT_PRETX is
  // refused in density mode).
  std::deque<DensitySlot> density_slots;
  bool hist = false;      // DS_JIT_HIST: histogram of fallback executions by pc
  bool fastcost = false;  // DS_JIT_FASTCOST: measurement knob (inexact data-cost arithmetic)
  // --cpu-oc (jit::set_cpu_oc): INEXACT opt-in tier. No per-access timing
  // lookup at all: every data access is priced at translate time as if it hit
  // main RAM (the ARM9 CPU-table entry / ARM7 bus entry of 0x02000000 for its
  // width), and the whole CD/CDI charge folds into the block's static cycles.
  bool cpu_oc = false;
  // DS_JIT_RETIME_ALL: a timing-table rebuild kills every ARM9 block (the old
  // rule) instead of only the blocks that baked a changed byte. A/B knob.
  bool retime_all = false;
  // DS_JIT_COSTPROBE_PART: which half of the per-access cost model the probe
  // duplicates -- 1 = the timing-table lookup, 2 = the combine arithmetic,
  // 3 (default) = both. Splits §A's price between the load and the maths.
  int  costprobe_part = 3;
  bool nocsel = false;    // DS_JIT_NOCSEL: branch around conditional data-processing instead
                          // of selecting, so the csel form can be A/B'd inside one binary.
  bool nocost7 = false;   // DS_JIT_NOCOST7: keep the inline ARM7 cost model, so the
                          // precomputed table can be A/B'd inside one binary.
  int  costprobe = 0;     // DS_JIT_COSTPROBE: 1 = both CPUs, 9 or 7 = that CPU only.
  // DS_JIT_MEMPROBE: emit the page-table walk (lsr / ldr / lsl) a second time
  // ahead of the real one, into the same scratch registers, so the real
  // sequence overwrites it and emulation is unchanged. The frame-time delta is
  // what the inline walk costs in instructions -- an upper bound on what
  // mapping guest memory into host address space could remove. It does NOT
  // price cache misses: the duplicate load always hits the line the real one
  // is about to touch, so a miss-dominated walk reads as cheaper than it is.
  int  memprobe = 0;      // 1 = both CPUs, 9 or 7 = that CPU only.
                          // Emit the data-cost sequence twice, the first copy's
                          // result discarded into a dead scratch. Semantics and frame output are
                          // unchanged (the budget is still charged exactly once), so the A/B runs
                          // the identical workload; the delta prices the per-access cost accounting.
  std::unordered_map<u64, u64> fallback_hist;
  Stats stats;
};

Runtime& rt();

// Runtime services used by the translator.
void   invalidate_host_page(const u8* host_page);
void   invalidate_host_range(const u8* host_page, const u8* lo, const u8* hi);
void   invalidate_cpu(JitCpu& jc);
void   lut_insert(JitCpu& jc, Block* b);
DensitySlot* density_new_slot();                 // null unless DS_JIT_DENSITY
Block* translate(JitCpu& jc, u32 key);          // null when the arena is full
const u8* find_native(JitCpu& jc, u32 key);      // translates on miss; null when arena is full
// translate.cpp: emit one block for `key` at the emitter's position. Returns
// false when the emitter ran out of room (the caller resets the arena).
bool   translate_block(JitCpu& jc, u32 key, Emitter& e, Block& b);

// Helpers called from translated code (through the stubs).
extern "C" {
u32         jit_h_fallback(CpuContext* cpu, u32 instr, u32 key);   // returns cpu->jumped
const void* jit_h_lookup(CpuContext* cpu, u32 key);
const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site);
void        jit_h_trace(CpuContext* cpu, u32 instr, u32 key);
void        jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key);
u32         jit_h_ld8(CpuContext* cpu, u32 addr);
u32         jit_h_ld16(CpuContext* cpu, u32 addr);
u32         jit_h_ld32(CpuContext* cpu, u32 addr);
void        jit_h_st8(CpuContext* cpu, u32 addr, u32 v);
void        jit_h_st16(CpuContext* cpu, u32 addr, u32 v);
void        jit_h_st32(CpuContext* cpu, u32 addr, u32 v);
}

// Flush the instruction cache for freshly written code.
inline void sync_icache(u8* start, size_t len) { __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(start + len)); }

} // namespace ds::jit
