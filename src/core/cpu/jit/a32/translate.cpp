// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) block translator. Semantics follow the interpreter
// (interp_arm.cpp / interp_thumb.cpp) instruction for instruction, including
// its cycle model (cpu_cycles.h); the block shape (block_shape.h), the budget
// test points and the static link are the AArch64 backend's, so its frame
// hashes are this one's oracle.
//
// What is different from a64/translate.cpp, and why:
//
//  - Guest registers are not all pinned. r0-r3 and sp live in r4-r8 (every
//    block and stub agrees; convention.h); the others live in memory and a
//    per-block cache (RegCache) keeps them in r0-r3/r12 while the block
//    runs: loaded on first read, written back before anything that can
//    observe memory (a stub call, the block end, a leave path). Slot choice
//    is LRU by instruction and deterministic, so a translation stays a pure
//    function of (key, guest bytes, timing stamp, CPU) as park-and-revive
//    requires.
//  - Guest NZCV (and Q) live in the host APSR, and the guest is a subset of
//    the host: a data-processing instruction is the guest's own encoding
//    with the register fields substituted -- shifter carry-out, RRX,
//    register-specified shifts and the v5TE multiplies are native, so the
//    flag-merge sequences of the A64 backend do not exist here.
//  - Conditional instructions are predicated, not branched around: the body
//    and its extra cycles carry the guest condition; the fetch cost is
//    charged ahead of the condition, as on A64 (`charged_ahead_`).
//  - A32 has no flag-neutral test, so the budget check saves and restores
//    the APSR when the flag-liveness pass says NZCV are live at that point.
//
//  - Memory instructions: the fast path probes the page table, tests the
//    entry (bracketed with mrs/msr only when the flags are live after the
//    instruction), does the access on the host-aligned address with the
//    guest's rotate or extend, looks the data cost up and charges it. The
//    cold path calls the slow helper, charges, polls and rejoins. Cache
//    rule for every cold path: it is emitted in the cache state of the
//    branch point and reconciles to the hot path's end state by reloading
//    that state's slots, so the hot path may evict freely after the branch.
//
// Phase 2, step 2: data processing, multiplies (plain, long, DSP), MRS, CLZ,
// static branches, CP15 no-ops, single loads/stores and same-page LDM/STM
// inline; indirect branches, LDM with pc, MSR and everything rarer go
// through the fallback stub, which is the interpreter.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/jit/a32/convention.h"
#include "core/cpu/jit/a32/emit.h"
#include "core/cpu/jit/block_shape.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu_cycles.h"
#include "core/nds.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace ds::jit {

namespace {

using arm::AOp;
using arm::TOp;

constexpr u32 MAX_INSTRS = 64;
constexpr size_t BLOCK_LIMIT = 28u << 10;
constexpr size_t COLD_CAP = 32u << 10;
constexpr u32 OFF_SPSR = offsetof(CpuContext, hot) + offsetof(JitHot, spsr);
constexpr u32 OFF_TIMING9 = offsetof(CpuContext, timing9);
constexpr u32 OFF_TIMING7 = offsetof(CpuContext, timing7);
constexpr u32 OFF_COST7   = offsetof(CpuContext, cost7);
static_assert(OFF_COST7 < 4096, "CpuContext timing pointers must be reachable from r11");

// Flag bits for the liveness pass.
constexpr u32 F_N = 8, F_Z = 4, F_C = 2, F_V = 1, F_ALL = 15;

u32 cond_reads(u32 cond) {
  switch (cond) {
  case 0x0: case 0x1: return F_Z;
  case 0x2: case 0x3: return F_C;
  case 0x4: case 0x5: return F_N;
  case 0x6: case 0x7: return F_V;
  case 0x8: case 0x9: return F_C | F_Z;
  case 0xA: case 0xB: return F_N | F_V;
  case 0xC: case 0xD: return F_N | F_Z | F_V;
  default:  return 0;
  }
}

struct FlagUse { u32 reads, writes; };

using shape::mcr_is_nop;

// What this backend hands to the interpreter. The liveness pass treats these
// as reading every flag (the stub syncs the APSR to memory), so the two must
// agree exactly: a block whose entry flags the pass calls dead skips the
// APSR save around the budget test.
bool arm_needs_fallback(u32 instr, bool a9) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) return !(((instr >> 25) & 7) == 5 && a9) && ((instr >> 24) & 0xF7) != 0x55;   // BLX imm (ARM9) and PLD inline
  const AOp op = arm::decode_arm(instr);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool test = opcode >= 8 && opcode <= 0xB;
    if (rd == 15 && !test) return true;
    return op == AOp::DpRegShift && ((instr >> 8) & 0xF) == 15;
  }
  case AOp::Mrs: case AOp::B: case AOp::Bl: case AOp::Pld:
    return false;
  case AOp::Mcr: return !mcr_is_nop(instr, a9);
  case AOp::Clz: return !a9;
  case AOp::Mul: case AOp::Mla:
    return rd == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 || rn == 15;
  case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    return rd == 15 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15;
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    return !a9 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 ||
           ((op == AOp::SmlaXY || op == AOp::SmlawY) && rd == 15);
  case AOp::LdrStrImm: case AOp::LdrStrReg: {
    const bool l = instr & (1u << 20), p = instr & (1u << 24), w = instr & (1u << 21);
    return (l && rd == 15) || (rn == 15 && (!p || w)) || (op == AOp::LdrStrReg && (instr & 0xF) == 15);
  }
  case AOp::LdrStrHImm: case AOp::LdrStrHReg: {
    const bool l = instr & (1u << 20), p = instr & (1u << 24), w = instr & (1u << 21);
    const u32 sh = (instr >> 5) & 3;
    return (!l && sh != 1) || rd == 15 || (rn == 15 && (!p || w)) || (op == AOp::LdrStrHReg && (instr & 0xF) == 15);
  }
  case AOp::Ldm: case AOp::Stm:
    return (instr & (1u << 22)) || (instr & 0xFFFF) == 0 || rn == 15 || (op == AOp::Ldm && (instr & 0x8000));
  default:
    return true;
  }
}

// `paired`: the previous instruction of the block is a BL prefix, so a BL/BLX
// suffix has a static target.
bool thumb_needs_fallback(u16 instr, bool a9, bool paired) {
  switch (arm::decode_thumb(instr)) {
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), op = (instr >> 8) & 3;
    return rd == 15 && (op == 0 || op == 2);
  }
  case TOp::ShiftImm: case TOp::AddSubReg: case TOp::AddSubImm3: case TOp::MovCmpAddSubImm8: case TOp::Alu:
  case TOp::B: case TOp::BCond: case TOp::BlPrefix: case TOp::AddPcSp: case TOp::AdjustSp:
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel:
    return false;
  case TOp::PushPop: return ((instr & 0xFF) | ((instr >> 8) & 1)) == 0 || ((instr & (1 << 11)) && (instr & (1 << 8)));   // empty, or pop pc
  case TOp::StmLdm: return (instr & 0xFF) == 0;
  case TOp::BlSuffix: return !paired;
  case TOp::BlxSuffix: return !a9 || !paired;
  default: return true;
  }
}

FlagUse arm_flag_use(u32 instr, bool a9) {
  const u32 cond = instr >> 28;
  if (arm_needs_fallback(instr, a9)) return {F_ALL, 0};
  if (cond == 0xF) return {0, 0};
  FlagUse u{0, 0};
  const AOp op = arm::decode_arm(instr);
  const bool s = instr & (1u << 20);
  const u32 opcode = (instr >> 21) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    if (opcode == 5 || opcode == 6 || opcode == 7) u.reads |= F_C;                 // ADC SBC RSC
    if (op == AOp::DpImmShift && ((instr >> 4) & 0xFF) == 0x06) u.reads |= F_C;    // RRX (ROR #0)
    if (s) {
      const bool arith = (opcode >= 2 && opcode <= 7) || opcode == 0xA || opcode == 0xB;
      if (arith) u.writes = F_ALL;
      else {
        u.writes = F_N | F_Z;
        bool carry_written;
        if (op == AOp::DpImm) carry_written = ((instr >> 8) & 0xF) != 0;
        else if (op == AOp::DpImmShift) carry_written = ((instr >> 7) & 0x1F) != 0 || ((instr >> 5) & 3) != 0;
        else { carry_written = true; u.reads |= F_C; }
        if (carry_written) u.writes |= F_C;
      }
    }
    break;
  }
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    if (s) u.writes = F_N | F_Z;   // the ARM7 also clears C; claiming less written is the safe side
    break;
  case AOp::Mrs: u.reads = F_ALL; break;
  case AOp::B: case AOp::Bl: case AOp::Clz: case AOp::Pld: case AOp::Mcr:
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: case AOp::Ldm: case AOp::Stm:
    break;
  default: u = {F_ALL, F_ALL}; break;
  }
  if (cond != 0xE) { u.reads |= cond_reads(cond); u.writes = 0; }
  return u;
}

FlagUse thumb_flag_use(u16 instr, bool a9, bool paired) {
  if (thumb_needs_fallback(instr, a9, paired)) return {F_ALL, 0};
  switch (arm::decode_thumb(instr)) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F;
    return {0, (type == 0 && amt == 0) ? (F_N | F_Z) : (F_N | F_Z | F_C)};
  }
  case TOp::AddSubReg: case TOp::AddSubImm3: return {0, F_ALL};
  case TOp::MovCmpAddSubImm8: return {0, ((instr >> 11) & 3) == 0 ? (F_N | F_Z) : F_ALL};
  case TOp::Alu:
    switch ((instr >> 6) & 0xF) {
    case 0x2: case 0x3: case 0x4: case 0x7: return {F_C, F_N | F_Z | F_C};
    case 0x5: case 0x6: return {F_C, F_ALL};
    case 0x9: case 0xA: case 0xB: return {0, F_ALL};
    default: return {0, F_N | F_Z};
    }
  case TOp::HiRegOp: return {0, ((instr >> 8) & 3) == 1 ? F_ALL : 0};
  case TOp::BCond: return {cond_reads((instr >> 8) & 0xF), 0};
  default: return {0, 0};
  }
}

struct Instr { u32 addr; u32 raw; u32 live_out; bool fallback; };

// ---- the register cache ---------------------------------------------------------------
// Guest registers that are not pinned live in JitHot::regs; a slot holds one
// of them for the rest of the block, or until the slot is needed. A slot is
// `dirty` when the block has written it. Slots used by the current
// instruction are locked so that allocating the next operand cannot evict
// one already chosen (at most five are ever needed at once). Temporaries
// come from the same pool.
class RegCache {
public:
  static constexpr u32 NSLOT = 5, NONE = 0xFF;
  static constexpr u32 HOST[NSLOT] = {SCRATCH0, SCRATCH1, SCRATCH2, SCRATCH3, SCRATCH4};
  struct Slot { u32 guest = NONE; bool dirty = false; u32 last = 0; };
  struct State { Slot slots[NSLOT]; };

  explicit RegCache(Emitter*& cur) : cur_(cur) {}

  void begin_instr() { ++tick_; locked_ = 0; }
  // Host register holding guest `g`, loading it first if it is not cached.
  u32 read(u32 g) {
    assert(g < 15);
    if (const u32 h = pinned_host(g); h != 0xFF) return h;
    int s = find(g);
    if (s < 0) {
      s = alloc();
      cur_->ldr(HOST[s], R_CTX, off_reg(g));
      slots_[s].guest = g;
      slots_[s].dirty = false;
    }
    return use(s);
  }
  // Host register to write guest `g` into. `keep_old`: the instruction reads
  // the old value first, or may not execute (predicated), so the slot must
  // start out holding it.
  u32 write(u32 g, bool keep_old) {
    assert(g < 15);
    if (const u32 h = pinned_host(g); h != 0xFF) return h;
    int s = find(g);
    if (s < 0) {
      s = alloc();
      if (keep_old) cur_->ldr(HOST[s], R_CTX, off_reg(g));
      slots_[s].guest = g;
    }
    slots_[s].dirty = true;
    return use(s);
  }
  // A temporary for the current instruction.
  u32 temp() {
    const int s = alloc();
    slots_[s].guest = NONE;
    slots_[s].dirty = false;
    return use(s);
  }
  void release(u32 host) {
    for (u32 s = 0; s < NSLOT; ++s) if (HOST[s] == host) { locked_ &= ~(1u << s); slots_[s] = Slot{}; }
  }
  // Keep a slot cached but let the rest of this instruction evict it.
  void unlock(u32 host) {
    for (u32 s = 0; s < NSLOT; ++s) if (HOST[s] == host) locked_ &= ~(1u << s);
  }
  int slot_of(u32 host) const {
    for (u32 s = 0; s < NSLOT; ++s) if (HOST[s] == host) return static_cast<int>(s);
    return -1;
  }
  // Load every cached slot from memory: a cold path rejoining the hot path
  // after the helpers may have clobbered the slot registers.
  void reload() {
    for (u32 s = 0; s < NSLOT; ++s)
      if (slots_[s].guest != NONE) cur_->ldr(HOST[s], R_CTX, off_reg(slots_[s].guest));
  }
  // Write every dirty slot back and forget everything: before a stub call,
  // the block end, or any point where memory must hold the guest state.
  void flush() {
    for (u32 s = 0; s < NSLOT; ++s) {
      if (slots_[s].guest != NONE && slots_[s].dirty) cur_->str(HOST[s], R_CTX, off_reg(slots_[s].guest));
      slots_[s] = Slot{};
    }
    locked_ = 0;
  }
  // Write dirty slots back without forgetting: a cold path that leaves.
  void writeback() {
    for (u32 s = 0; s < NSLOT; ++s)
      if (slots_[s].guest != NONE && slots_[s].dirty) cur_->str(HOST[s], R_CTX, off_reg(slots_[s].guest));
  }
  bool empty() const {
    for (u32 s = 0; s < NSLOT; ++s) if (slots_[s].guest != NONE) return false;
    return true;
  }
  State save() const { State st; for (u32 s = 0; s < NSLOT; ++s) st.slots[s] = slots_[s]; return st; }
  void restore(const State& st) { for (u32 s = 0; s < NSLOT; ++s) slots_[s] = st.slots[s]; locked_ = 0; }

private:
  int find(u32 g) const {
    for (u32 s = 0; s < NSLOT; ++s) if (slots_[s].guest == g) return static_cast<int>(s);
    return -1;
  }
  u32 use(int s) { slots_[s].last = tick_; locked_ |= 1u << s; return HOST[s]; }
  // A free unlocked slot (lowest first), else the least recently used
  // unlocked one, written back if dirty.
  int alloc() {
    for (u32 s = 0; s < NSLOT; ++s) if (!(locked_ & (1u << s)) && slots_[s].guest == NONE) return static_cast<int>(s);
    int best = -1;
    for (u32 s = 0; s < NSLOT; ++s) {
      if (locked_ & (1u << s)) continue;
      if (best < 0 || slots_[s].last < slots_[best].last) best = static_cast<int>(s);
    }
    assert(best >= 0 && "register cache: every slot is locked");
    if (slots_[best].dirty) cur_->str(HOST[best], R_CTX, off_reg(slots_[best].guest));
    slots_[best] = Slot{};
    return best;
  }

  Emitter*& cur_;
  Slot slots_[NSLOT];
  u32 tick_ = 0, locked_ = 0;
};

class Translator {
public:
  // The cold-section scratch is reused across translations (see a64).
  static u8* cold_scratch() {
    static thread_local std::unique_ptr<u8[]> buf;
    if (!buf) buf.reset(new u8[COLD_CAP]);
    return buf.get();
  }
  Translator(JitCpu& jc, u32 key, Emitter& e, Block& b)
      : jc_(jc), cpu_(*jc.ctx), hot_(e), blk_(b), key_(key), thumb_(key_thumb(key)), a9_(jc.arm9),
        cold_buf_(cold_scratch()), cold_(cold_buf_, COLD_CAP), cur_(&hot_), cache_(cur_) {}

  bool run();

private:
  JitCpu& jc_;
  CpuContext& cpu_;
  Emitter& hot_;
  Block& blk_;
  const u32 key_;
  const bool thumb_, a9_;
  std::vector<Instr> instrs_;
  u32 pc_ = 0;
  u32 live_ = F_ALL;
  Cond cond_ = AL;           // condition of the instruction being emitted (predication)
  u32 pending_ = 0;          // static cycles not yet subtracted from the budget
  u32 charged_ahead_ = 0;    // cycles already charged before a conditional instruction's body
  bool ended_ = false;
  const u8* t7_ = nullptr;
  u32 code_region7_ = 0;
  bool bl_prefix_valid_ = false;
  u32  bl_prefix_lr_ = 0;

  u8* cold_buf_;
  Emitter cold_;
  Emitter* cur_;
  RegCache cache_;
  struct Fix { size_t at; bool at_cold; size_t target; bool target_cold; const void* abs; };
  std::vector<Fix> fixes_;

  Emitter& e() { return *cur_; }
  bool in_cold() const { return cur_ == &cold_; }
  u32 step() const { return thumb_ ? 2 : 4; }

  // ---- hot / cold sections ------------------------------------------------------------
  void call_stub(const u8* stub) {
    if (!in_cold()) { hot_.bl(stub); return; }
    fixes_.push_back({cold_.bl_fwd(), true, 0, false, stub});
  }
  void cold_begin(const std::vector<size_t>& hot_fixups) {
    assert(!in_cold());
    for (size_t f : hot_fixups) fixes_.push_back({f, false, cold_.size(), true, nullptr});
    cur_ = &cold_;
  }
  void cold_end() { cur_ = &hot_; }
  void cold_end_jump(size_t hot_target) {
    fixes_.push_back({cold_.b_fwd(), true, hot_target, false, nullptr});
    cur_ = &hot_;
  }
  bool finish() {
    assert(!in_cold());
    const size_t cold_base = hot_.size();
    blk_.hot_size = static_cast<u32>(cold_base);
    if (hot_.remaining() < cold_.size() + 64) return false;
    std::memcpy(hot_.cur(), cold_buf_, cold_.size());
    hot_.set_pos(cold_base + cold_.size());
    u8* base = hot_.base();
    for (const Fix& f : fixes_) {
      u8* at = base + (f.at_cold ? cold_base + f.at : f.at);
      const u8* target = f.abs ? static_cast<const u8*>(f.abs) : base + (f.target_cold ? cold_base + f.target : f.target);
      Emitter::patch_rel(at, target);
    }
    assert(hot_.size() >= backend::ENTRY_PATCH && "kill_block patches 12 bytes at the entry");
    return true;
  }

  // ---- decoding -------------------------------------------------------------------------
  u32 fetch(u32 addr) {
    if (u8* p = cpu_.page_table.read_ptr(addr)) {
      if (thumb_) { u16 v; std::memcpy(&v, p, 2); return v; }
      u32 v; std::memcpy(&v, p, 4); return v;
    }
    return thumb_ ? cpu_.nds->bus.read16(cpu_.which, addr) : cpu_.nds->bus.read32(cpu_.which, addr);
  }

  // ---- cycles ----------------------------------------------------------------------------
  void note_dep(u32 addr, u8 kind) const {
    const u32 page = addr >> 12;
    for (u32 i = 0; i < blk_.ndep; ++i) if (blk_.dep_page[i] == page) { blk_.dep_kind[i] |= kind; return; }
    if (blk_.ndep < Block::DEP_MAX) { blk_.dep_page[blk_.ndep] = page; blk_.dep_kind[blk_.ndep] = kind; ++blk_.ndep; }
    else blk_.dep_overflow = true;
  }
  u32 numC(u32 addr) const {
    if (a9_) {
      const u32 pf = addr + (thumb_ ? 4 : 8);
      if (thumb_ && (pf & 2)) return 0;
      note_dep(pf, mem::Timing::RETIME_CODE);
      return fetch_cost9(cpu_, pf, false);
    }
    return t7_[thumb_ ? 1 : 3];
  }
  u32 numC_nonseq7() const { return t7_[thumb_ ? 0 : 2]; }
  // The whole CD/CDI charge for a data cost known at translate time (a
  // pc-relative literal, or every access under --cpu-oc); mirrors the
  // run-time formulas below.
  u32 const_charge(u32 nd, bool cdi) const {
    const s32 d = static_cast<s32>(nd);
    if (a9_) { const s32 nc = static_cast<s32>(numC(pc_)); return max3(nc + d - 6, nc, d); }
    const s32 nc = static_cast<s32>(numC_nonseq7());
    if (code_region7_ == 0x02) return static_cast<u32>(d + nc);
    const s32 ncx = nc + (cdi ? 1 : 0);
    return max3(ncx, d, d + ncx - 3);
  }
  u32 oc_data_cost(bool word, bool seq, bool store) const {
    if (a9_) return cpu_.timing9[0x02000000u >> 12][(store ? 4 : 0) + (seq ? 3 : (word ? 2 : 1))];
    return cpu_.timing7[0x02000000u >> 15][seq ? (word ? 3 : 1) : (word ? 2 : 0)];
  }
  // Slot in mem::Timing's precomputed ARM7 cost table for a single access
  // with these translate-time constants, or -1 when the table does not cover
  // this block's code-fetch cost (the instruction then stays a fallback).
  int cost7_slot(bool cdi, bool word) const {
    if (a9_) return -1;
    const int ni = cpu_.nds->bus.timing().nc7_index(numC_nonseq7());
    if (ni < 0) return -1;
    return static_cast<int>(mem::Timing::cost7_offset(code_region7_ == 0x02, cdi, static_cast<u32>(ni), word));
  }
  // An ARM7 single access this translation cannot price from the table.
  bool needs_fallback_cost7(u32 raw) const {
    if (a9_) return false;
    if (thumb_) {
      switch (arm::decode_thumb(static_cast<u16>(raw))) {
      case TOp::LdrPcRel: return cost7_slot(true, true) < 0;
      case TOp::LdrStrReg: { const u32 o = (raw >> 9) & 7; return cost7_slot(o >= 3, o == 0 || o == 4) < 0; }
      case TOp::LdrStrImm5: return cost7_slot(raw & (1 << 11), !(raw & (1 << 12))) < 0;
      case TOp::LdrStrHImm5: return cost7_slot(raw & (1 << 11), false) < 0;
      case TOp::LdrStrSpRel: return cost7_slot(raw & (1 << 11), true) < 0;
      default: return false;
      }
    }
    switch (arm::decode_arm(raw)) {
    case AOp::LdrStrImm: case AOp::LdrStrReg: return cost7_slot(raw & (1u << 20), !(raw & (1u << 22))) < 0;
    case AOp::LdrStrHImm: case AOp::LdrStrHReg: return cost7_slot(raw & (1u << 20), false) < 0;
    default: return false;
    }
  }
  u32 numC_internal() const { return a9_ ? numC(pc_) : numC_nonseq7(); }
  void add_pending(u32 c) {
    if (charged_ahead_) { const u32 k = std::min(c, charged_ahead_); c -= k; charged_ahead_ -= k; }
    pending_ += c;
  }
  // Subtract the pending cycles, predicated on the current instruction's
  // condition (a conditional instruction's extra cycles beyond the fetch).
  void flush_pending() {
    if (!pending_) return;
    u32 i;
    if (encode_imm12(pending_, i)) e().dp_imm(SUB, false, R_BUDGET, R_BUDGET, i, cond_);
    else {
      const u32 t = cache_.temp();
      e().mov_imm(t, pending_, cond_);
      e().sub_reg(R_BUDGET, R_BUDGET, t, LSL, 0, cond_);
      cache_.release(t);
    }
    pending_ = 0;
  }
  // Leave with `exit_key` when the budget is exhausted. The test clobbers
  // NZCV: when the guest flags are live here they ride in a temporary.
  void emit_budget_check(u32 exit_key, bool flags_live) {
    assert(!in_cold());
    u32 t = 0;
    if (flags_live) { t = cache_.temp(); hot_.mrs_apsr(t); }
    hot_.tst_imm(R_BUDGET, 0x80000000u);
    const size_t br = hot_.b_fwd(NE);
    if (flags_live) hot_.msr_apsr_nzcvq(t);
    cold_begin({br});
    if (flags_live) cold_.msr_apsr_nzcvq(t);
    cache_.writeback();
    call_stub(rt().exit_key_lit);
    cold_.word(exit_key);
    cold_end();
    if (flags_live) cache_.release(t);
  }

  // ---- helpers: fallback, trace -----------------------------------------------------------
  // Run this instruction through the interpreter (it evaluates the condition
  // itself and charges its own cycles). The stub polls, returns when the
  // instruction did not jump and dispatches when it did.
  void emit_fallback(u32 instr, bool always_jumps) {
    cache_.flush();
    flush_pending();
    call_stub(jc_.fallback);
    e().word(instr);
    e().word(make_key(pc_, thumb_));
    if (always_jumps) ended_ = true;
  }
  void emit_call2(const void* fn, u32 instr, u32 key) {
    cache_.flush();
    flush_pending();
    call_stub(rt().call2);
    e().word(instr);
    e().word(key);
    e().word(static_cast<u32>(reinterpret_cast<uintptr_t>(fn)));
  }

  // ---- operands -----------------------------------------------------------------------------
  u32 pc_const(u32 value) {
    const u32 t = cache_.temp();
    e().mov_imm(t, value);
    return t;
  }
  u32 reg_read(u32 r, u32 pc_value) { return r == 15 ? pc_const(pc_value) : cache_.read(r); }
  // A slot register that is neither of two others (cold paths, where every
  // slot register but the named ones is free).
  static u32 free_reg_except(u32 a, u32 b) {
    for (u32 r : RegCache::HOST) if (r != a && r != b) return r;
    return SCRATCH4;
  }
  static u32 free_reg_except3(u32 a, u32 b, u32 c) {
    for (u32 r : RegCache::HOST) if (r != a && r != b && r != c) return r;
    return SCRATCH4;
  }

  // ---- memory -----------------------------------------------------------------------------------
  enum class Mem { Ld32, Ld16, Ld8, Ld16S, Ld8S, St32, St16, St8 };
  static bool is_load(Mem m) { return m <= Mem::Ld8S; }
  static bool is_word(Mem m) { return m == Mem::Ld32 || m == Mem::St32; }
  static u32 size_index(Mem m) {
    switch (m) {
    case Mem::Ld8: case Mem::Ld8S: case Mem::St8: return 0;
    case Mem::Ld16: case Mem::Ld16S: case Mem::St16: return 1;
    default: return 2;
    }
  }
  // The guest flags around a flag-clobbering test sequence: saved into a
  // temporary when live, restored by `flags_end` (the cold path restores
  // them itself from the same register).
  u32 flags_begin() {
    if (!live_) return 0xFF;
    const u32 f = cache_.temp();
    e().mrs_apsr(f);
    return f;
  }
  void flags_end(u32 f) {
    if (f == 0xFF) return;
    e().msr_apsr_nzcvq(f);
    cache_.release(f);
  }
  // Data cost of one access at `a` into `c`, through the raw timing table
  // (`t` is a temporary). ARM9 entries are 8 bytes (loads [1..3], stores
  // [5..7]); ARM7 entries are 4 bytes.
  void emit_data_cost(u32 a, u32 c, u32 t, bool word, bool seq, bool store) {
    const u32 k = a9_ ? ((store ? 4 : 0) + (seq ? 3 : (word ? 2 : 1))) : (seq ? (word ? 3 : 1) : (word ? 2 : 0));
    e().ldr(t, R_CTX, a9_ ? OFF_TIMING9 : OFF_TIMING7);
    e().lsr_imm(c, a, a9_ ? 12 : 15);
    e().add_reg(c, t, c, LSL, a9_ ? 3 : 2);
    e().ldrb(c, c, k);
  }
  // The ARM7 single-access cost from the precomputed table (cost7_slot >= 0).
  void emit_cost7(u32 a, u32 c, u32 t, int slot) {
    e().ldr(t, R_CTX, OFF_COST7);
    e().lsr_imm(c, a, 15);
    e().add_reg(c, t, c, LSL, 5);
    e().ldrb(c, c, static_cast<u32>(slot));
  }
  // t = max(t, u) without flags: t + max(u - t, 0)
  void emit_max_into(u32 t, u32 u, u32 tmp) {
    e().sub_reg(tmp, u, t);
    e().dp_reg(BIC, false, tmp, tmp, tmp, ASR, 31);
    e().add_reg(t, t, tmp);
  }
  // Charge a CD / CDI instruction with data cost `c` at address `a`; `t0`
  // and `t1` are temporaries (the ARM7 needs both and clobbers `c`). Every
  // instruction here is flag-neutral.
  void emit_charge(u32 c, u32 a, u32 t0, u32 t1, bool cdi) {
    if (a9_) {
      // cost = max(nc + nd - 6, nc, nd), nd >= 1: nc <= 1 -> nd; nc <= 6 -> max(nc, nd); else nc + max(nd - 6, 0)
      const u32 nc = numC(pc_);
      if (nc <= 1) { e().sub_reg(R_BUDGET, R_BUDGET, c); return; }
      e().sub_imm(t0, c, nc <= 6 ? nc : 6, t1);
      e().dp_reg(BIC, false, t0, t0, t0, ASR, 31);
      e().add_imm(t0, t0, nc, t1);
      e().sub_reg(R_BUDGET, R_BUDGET, t0);
      return;
    }
    // ARM7 (cpu_cycles.h charge_CD / charge_CDI): the one dynamic input is
    // whether the data is in main RAM. Costs add when data and code share
    // that region (cost = d + k), overlap into a max when they do not
    // (cost = max(x, d + y, x + d + y - 3)); which of the two applies is
    // data_main != code_main, and x, y, k are translate-time constants.
    // Both candidates are computed and a mask selects; `a` is used up.
    const u32 nc = numC_nonseq7();
    const bool code_main = code_region7_ == 0x02;
    const u32 k = nc + ((cdi && !code_main) ? 1 : 0);
    const u32 x = nc + ((cdi && !code_main) ? 1 : 0), y = (cdi && code_main) ? 1 : 0;
    e().lsr_imm(t0, a, 24);
    e().sub_imm(t0, t0, 2, t1);
    e().clz(t0, t0);
    e().lsr_imm(t0, t0, 5);                                   // t0 = data_main (0/1)
    if (code_main) e().eor_imm(t0, t0, 1);                    // t0 = 1 when the max form applies
    e().add_imm(a, c, y, a);                                  // a = d + y
    e().sub_imm(t1, a, x, t1);                                // t1 = max(d + y, x)
    e().dp_reg(BIC, false, t1, t1, t1, ASR, 31);
    e().add_imm(t1, t1, x, t1);
    if (x >= 3) e().add_imm(a, a, x - 3, a); else e().sub_imm(a, a, 3 - x, a);   // a = x + d + y - 3
    e().sub_reg(a, a, t1);                                    // t1 = max(t1, a)
    e().dp_reg(BIC, false, a, a, a, ASR, 31);
    e().add_reg(t1, t1, a);
    e().add_imm(c, c, k, a);                                  // c = d + k (the add form)
    e().sub_reg(t1, t1, c);
    e().dp_imm(RSB, false, t0, t0, 0);                        // t0 = 0 / -1
    e().and_reg(t1, t1, t0);
    e().add_reg(c, c, t1);
    e().sub_reg(R_BUDGET, R_BUDGET, c);
  }
  // Slow-path epilogue: the helper returned the aligned raw value in `v`;
  // reproduce the rotation / extension in place (`addr`, `tmp` scratch).
  void emit_load_post(Mem m, u32 v, u32 addr, u32 tmp) {
    switch (m) {
    case Mem::Ld32: e().lsl_imm(tmp, addr, 3); e().ror_reg(v, v, tmp); break;
    case Mem::Ld16: if (!a9_) { e().and_imm(tmp, addr, 1); e().lsl_imm(tmp, tmp, 3); e().ror_reg(v, v, tmp); } break;
    case Mem::Ld8S: e().sxtb(v, v); break;
    case Mem::Ld16S:
      e().sxth(v, v);
      if (!a9_) { e().and_imm(tmp, addr, 1); e().lsl_imm(tmp, tmp, 3); e().asr_reg(v, v, tmp); }
      break;
    default: break;
    }
  }
  // r1 = a, r2 = data (0xFF: none), through r3 when the two cross.
  void emit_slow_args(u32 a, u32 data) {
    if (data == 0xFF) { if (a != 1) e().mov(1, a); return; }
    if (a == 1 && data == 2) return;
    if (a == 2 && data == 1) { e().mov(3, 1); e().mov(1, 2); e().mov(2, 3); return; }
    if (data == 1) { e().mov(2, 1); if (a != 1) e().mov(1, a); return; }
    if (a == 2) { e().mov(1, 2); if (data != 2) e().mov(2, data); return; }
    if (a != 1) e().mov(1, a);
    if (data != 2) e().mov(2, data);
  }

  // One load or store at the address in `a` (a locked temporary; the base
  // writeback, if any, is already in its slot). Stores take the data in
  // `data`; loads write guest `rd`. `const_nd` >= 0: the data cost is a
  // translate-time constant (a pc-relative literal's page, or --cpu-oc) and
  // joins the static cycles.
  void emit_single(Mem m, u32 a, u32 data, u32 rd, bool cdi, int const_nd = -1) {
    const bool load = is_load(m), word = is_word(m);
    if (rt().cpu_oc && const_nd < 0) const_nd = static_cast<int>(oc_data_cost(word, false, !load));
    const bool const_cost = const_nd >= 0;
    if (const_cost) add_pending(const_charge(static_cast<u32>(const_nd), cdi));
    flush_pending();
    const int slot7 = const_cost ? -1 : cost7_slot(cdi, word);
    assert(a9_ || const_cost || slot7 >= 0);
    // ---- hot path ----
    const u32 f = flags_begin();
    const u32 en = cache_.temp();
    const RegCache::State s0 = cache_.save();
    std::vector<size_t> fail;
    e().lsr_imm(en, a, mem::PAGE_SHIFT);
    e().ldr_reg(en, R_PT, en, LSL, 2);
    if (!load) { e().tst_imm(en, 0xC0000000u); fail.push_back(e().b_fwd(NE)); }
    e().dp_reg(MOV, true, en, 0, en, LSL, 2);          // biased host base; 0 = unmapped
    fail.push_back(e().b_fwd(EQ));
    flags_end(f);
    e().add_reg(en, en, a);                            // host address (base is 4-aligned)
    u32 d = 0;
    if (load) d = cache_.write(rd, false);
    switch (m) {
    case Mem::Ld32: e().and_imm(en, en, ~3u); e().ldr(d, en, 0); e().lsl_imm(en, a, 3); e().ror_reg(d, d, en); break;
    case Mem::Ld16:
      e().and_imm(en, en, ~1u); e().ldrh(d, en, 0);
      if (!a9_) { e().and_imm(en, a, 1); e().lsl_imm(en, en, 3); e().ror_reg(d, d, en); }
      break;
    case Mem::Ld8:  e().ldrb(d, en, 0); break;
    case Mem::Ld8S: e().ldrsb(d, en, 0); break;
    case Mem::Ld16S:
      e().and_imm(en, en, ~1u); e().ldrsh(d, en, 0);
      if (!a9_) { e().and_imm(en, a, 1); e().lsl_imm(en, en, 3); e().asr_reg(d, d, en); }   // odd: the signed high byte
      break;
    case Mem::St32: e().and_imm(en, en, ~3u); e().str(data, en, 0); break;
    case Mem::St16: e().and_imm(en, en, ~1u); e().strh(data, en, 0); break;
    case Mem::St8:  e().strb(data, en, 0); break;
    }
    if (!const_cost) {
      const u32 c = cache_.temp();
      if (a9_) { emit_data_cost(a, c, en, word, false, !load); emit_charge(c, a, en, en, cdi); }
      else { emit_cost7(a, c, en, slot7); e().sub_reg(R_BUDGET, R_BUDGET, c); }
      cache_.release(c);
    }
    cache_.release(en);
    const size_t join = hot_.size();
    const RegCache::State s1 = cache_.save();
    // ---- cold path: the helper, then the same tail, then rejoin ----
    cold_begin(fail);
    if (f != 0xFF) e().msr_apsr_nzcvq(f);
    cache_.restore(s0);
    cache_.writeback();
    emit_slow_args(a, load ? 0xFF : data);
    call_stub(load ? rt().slow_load[size_index(m)] : rt().slow_store[size_index(m)]);
    if (load) {
      emit_load_post(m, 0, 1, 2);
      if (const u32 h = pinned_host(rd); h != 0xFF) e().mov(h, 0); else e().str(0, R_CTX, off_reg(rd));
    }
    if (!const_cost) {
      if (a9_) { emit_data_cost(1, 2, 3, word, false, !load); emit_charge(2, 1, 3, SCRATCH4, cdi); }
      else { emit_cost7(1, 2, 3, slot7); e().sub_reg(R_BUDGET, R_BUDGET, 2); }
    }
    call_stub(rt().poll);
    e().word(make_key(pc_ + step(), thumb_));
    cache_.restore(s1);
    cache_.reload();
    cold_end_jump(join);
  }

  // Multi-register transfer, fast path in one 2 KB page; the cold path is
  // the interpreter, so rn must still hold its old value there: the
  // writeback value waits in `t_w` (a locked temp; 0xFF: it equals `a`) and
  // reaches rn after the transfer. `a` = start address (a locked temp).
  void emit_block_transfer(u32 instr, u32 list, bool load, bool writeback, u32 rn, u32 a, u32 t_w, u32 pc_store_value) {
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    flush_pending();
    const u32 f = flags_begin();
    const u32 en = cache_.temp(), e2 = cache_.temp();
    const RegCache::State s0 = cache_.save();
    std::vector<size_t> fail;
    e().add_imm(e2, a, n * 4 - 1, e2);
    e().eor_reg(e2, e2, a);
    e().dp_reg(MOV, true, e2, 0, e2, LSR, mem::PAGE_SHIFT);
    fail.push_back(e().b_fwd(NE));
    e().lsr_imm(en, a, mem::PAGE_SHIFT);
    e().ldr_reg(en, R_PT, en, LSL, 2);
    if (!load) { e().tst_imm(en, 0xC0000000u); fail.push_back(e().b_fwd(NE)); }
    e().dp_reg(MOV, true, en, 0, en, LSL, 2);
    fail.push_back(e().b_fwd(EQ));
    flags_end(f);
    cache_.release(e2);
    e().add_reg(en, en, a);
    e().and_imm(en, en, ~3u);                          // each word access is aligned; the base may not be (Thumb)
    u32 k = 0;
    bool first = true;
    const u32 wv = t_w == 0xFF ? a : t_w;
    if (load) {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        const u32 h = cache_.write(i, false);
        e().ldr(h, en, 4 * k);
        cache_.unlock(h);
        ++k;
      }
      if (writeback && !(list & (1u << rn))) {         // a loaded rn wins over the writeback
        const u32 h = cache_.write(rn, false);
        e().mov(h, wv);
        cache_.unlock(h);
      }
    } else {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        u32 src;
        bool tmp = false;
        if (i == 15) { src = cache_.temp(); e().mov_imm(src, pc_store_value); tmp = true; }
        else if (i == rn && !first && writeback) src = wv;
        else src = cache_.read(i);
        e().str(src, en, 4 * k);
        if (tmp) cache_.release(src); else if (src != a && src != t_w) cache_.unlock(src);
        ++k; first = false;
      }
      if (writeback) {
        const u32 h = cache_.write(rn, false);
        e().mov(h, wv);
        cache_.unlock(h);
      }
    }
    if (t_w != 0xFF) cache_.release(t_w);
    cache_.release(en);
    // cost: N + (n - 1) S from the page's entry (--cpu-oc: main RAM's, at translate time)
    if (rt().cpu_oc) {
      const u32 nd = oc_data_cost(true, false, !load) + (n - 1) * oc_data_cost(true, true, !load);
      add_pending(const_charge(nd, load));
      flush_pending();
    } else {
      const u32 c = cache_.temp(), t = cache_.temp();
      emit_data_cost(a, c, t, true, false, !load);
      if (n > 1) {
        const u32 c2 = cache_.temp();
        emit_data_cost(a, c2, t, true, true, !load);
        e().mov_imm(t, n - 1);
        e().mla(c, c2, t, c);
        cache_.release(c2);
      }
      const u32 t1 = cache_.temp();
      emit_charge(c, a, t, t1, load);
      cache_.release(t1);
      cache_.release(t);
      cache_.release(c);
    }
    const size_t join = hot_.size();
    const RegCache::State s1 = cache_.save();
    cold_begin(fail);
    if (f != 0xFF) e().msr_apsr_nzcvq(f);
    cache_.restore(s0);
    emit_fallback(instr, false);
    cache_.restore(s1);
    cache_.reload();
    cold_end_jump(join);
  }

  // Effective address of a single transfer into a fresh temporary. The base
  // writeback goes to rn's slot right away, so the cold path's state has it
  // (a load into rn then overwrites it, as the guest does). A store's data
  // register is copied first when the writeback would clobber it.
  u32 emit_ea(u32 rn, bool off_imm, u32 off_val, u32 off_host, Shift sh, u32 amt, bool pre, bool up, bool wb, u32& data) {
    const u32 base = reg_read(rn, pc_ + 8);
    const u32 a = cache_.temp();
    auto addoff = [&](u32 dst, u32 src) {
      if (off_imm) { if (up) e().add_imm(dst, src, off_val, dst); else e().sub_imm(dst, src, off_val, dst); }
      else e().dp_reg(up ? ADD : SUB, false, dst, src, off_host, sh, amt);
    };
    if (pre) addoff(a, base); else e().mov(a, base);
    if (wb) {
      if (data != 0xFF && data == base) { const u32 t = cache_.temp(); e().mov(t, data); data = t; }
      const u32 w = cache_.write(rn, false);
      if (pre) e().mov(w, a); else addoff(w, a);
      cache_.unlock(w);
    }
    if (rn != 15 && base != data) cache_.unlock(base);
    if (!off_imm && off_host != data) cache_.unlock(off_host);
    return a;
  }

  // ---- branches -------------------------------------------------------------------------------
  void emit_branch_static(u32 target, bool to_thumb, bool refill) {
    assert(cond_ == AL);
    if (refill) {
      if (a9_) {   // the pages refill_cycles reads (cpu_cycles.h)
        if (!to_thumb) { note_dep(target, mem::Timing::RETIME_CODE); note_dep(target + 4, mem::Timing::RETIME_CODE); }
        else if (target & 2) { note_dep(target - 2, mem::Timing::RETIME_CODE); note_dep(target + 2, mem::Timing::RETIME_CODE); }
        else note_dep(target, mem::Timing::RETIME_CODE);
      }
      add_pending(refill_cycles(cpu_, target, to_thumb));
    }
    cache_.flush();
    flush_pending();
    if (to_thumb != thumb_) {
      e().ldr(SCRATCH0, R_CTX, OFF_CPSR);
      e().eor_imm(SCRATCH0, SCRATCH0, 0x20);
      e().str(SCRATCH0, R_CTX, OFF_CPSR);
    }
    call_stub(jc_.link);
    e().word(make_key(target, to_thumb));
    if (blk_.nsucc < 4) blk_.succ[blk_.nsucc++] = make_key(target, to_thumb);
    ended_ = true;
  }
  // A conditional static branch: both arms end the block. `taken` emits the
  // lr write (if any) and the branch; the fall-through arm charges numC and
  // links to the next instruction.
  template <class Taken>
  void emit_branch_cond(u32 cond, Taken taken) {
    assert(cond_ == AL);
    cache_.flush();
    flush_pending();
    const size_t skip = hot_.b_fwd(invert(static_cast<Cond>(cond)));
    const RegCache::State st = cache_.save();
    taken();
    hot_.bind(skip);
    cache_.restore(st);
    ended_ = false;
    add_pending(numC(pc_));
    emit_branch_static(pc_ + step(), thumb_, false);
  }

  // ---- multiplies -----------------------------------------------------------------------------
  // ARM7: 1..4 internal cycles from the magnitude of rs (signed: of rs ^ (rs >> 31)).
  void emit_mul_cycles7(u32 hrs, bool signed_op, u32 extra) {
    const u32 t = cache_.temp(), u = cache_.temp();
    if (signed_op) e().eor_reg(t, hrs, hrs, ASR, 31, cond_); else e().mov(t, hrs, cond_);
    e().clz(t, t, cond_);
    e().lsr_imm(t, t, 3, cond_);            // leading zero bytes 0..4
    e().lsl_imm(t, t, 2, cond_);
    e().mov_imm(u, 0x11234, cond_);         // nibble table: 4,3,2,1,1
    e().lsr_reg(u, u, t, cond_);
    e().and_imm(u, u, 0xF, cond_);
    if (extra) e().add_imm(u, u, extra, u, false, cond_);
    e().sub_reg(R_BUDGET, R_BUDGET, u, LSL, 0, cond_);
    cache_.release(t);
    cache_.release(u);
  }
  // Multiplies that set flags leave C alone on the ARM9 and clear it on the
  // ARM7 (melonDS); the host leaves it alone. Never predicated: the multiply
  // has rewritten N and Z by now (translate_arm uses the branch form).
  bool mul_clears_c(u32 instr) const { return !a9_ && (instr & (1u << 20)) && (live_ & F_C); }
  void emit_clear_c() {
    if (a9_ || !(live_ & F_C)) return;
    assert(cond_ == AL);
    const u32 t = cache_.temp();
    e().mrs_apsr(t, cond_);
    e().and_imm(t, t, ~0x20000000u, cond_);
    e().msr_apsr_nzcvq(t, cond_);
    cache_.release(t);
  }

  // ---- drivers ----------------------------------------------------------------------------------
  void translate_arm(u32 instr, bool fb);
  void translate_thumb(u16 instr, bool fb);
  void arm_data_processing(u32 instr, AOp op);
  void arm_multiply(u32 instr, AOp op);
  void arm_dsp_multiply(u32 instr, AOp op);
  void arm_ldr_str(u32 instr, AOp op);
  void arm_ldr_str_h(u32 instr, AOp op);
  void arm_ldm_stm(u32 instr, bool load);
  void thumb_alu(u16 instr);
  void thumb_ldr_str(u16 instr, TOp op);
  void thumb_push_pop(u16 instr);
  void thumb_stm_ldm(u16 instr);
};

// ---- memory instructions ------------------------------------------------------------------------

void Translator::arm_ldr_str(u32 instr, AOp op) {
  const bool l = instr & (1u << 20), b = instr & (1u << 22);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const bool writeback = !p || w;
  u32 data = 0xFF;
  if (!l) data = rd == 15 ? pc_const(pc_ + 12) : cache_.read(rd);
  u32 a;
  if (op == AOp::LdrStrImm) a = emit_ea(rn, true, instr & 0xFFF, 0, LSL, 0, p, u, writeback, data);
  else a = emit_ea(rn, false, 0, cache_.read(instr & 0xF), static_cast<Shift>((instr >> 5) & 3), (instr >> 7) & 0x1F, p, u, writeback, data);
  int const_nd = -1;
  if (l && a9_ && !b && op == AOp::LdrStrImm && rn == 15 && !writeback) {
    // ldr rd, [pc, #imm]: the literal's page, hence its N32 cost, is known now.
    const u32 addr = u ? pc_ + 8 + (instr & 0xFFF) : pc_ + 8 - (instr & 0xFFF);
    note_dep(addr, mem::Timing::RETIME_DATA);
    const_nd = cpu_.timing9[addr >> 12][2];
  }
  if (l) emit_single(b ? Mem::Ld8 : Mem::Ld32, a, 0xFF, rd, true, const_nd);
  else emit_single(b ? Mem::St8 : Mem::St32, a, data, 0, false);
  cache_.release(a);
}

void Translator::arm_ldr_str_h(u32 instr, AOp op) {
  const bool l = instr & (1u << 20);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 sh = (instr >> 5) & 3;
  const bool writeback = !p || w;
  u32 data = 0xFF;
  if (!l) data = cache_.read(rd);
  u32 a;
  if (op == AOp::LdrStrHImm) a = emit_ea(rn, true, ((instr >> 4) & 0xF0) | (instr & 0xF), 0, LSL, 0, p, u, writeback, data);
  else a = emit_ea(rn, false, 0, cache_.read(instr & 0xF), LSL, 0, p, u, writeback, data);
  if (l) emit_single(sh == 1 ? Mem::Ld16 : sh == 2 ? Mem::Ld8S : Mem::Ld16S, a, 0xFF, rd, true);
  else emit_single(Mem::St16, a, data, 0, false);
  cache_.release(a);
}

void Translator::arm_ldm_stm(u32 instr, bool load) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  const u32 list = instr & 0xFFFF;
  const u32 n = static_cast<u32>(__builtin_popcount(list));
  const u32 hb = cache_.read(rn);
  const u32 a = cache_.temp();
  if (u) { if (p) e().add_imm(a, hb, 4, a); else e().mov(a, hb); }
  else   { if (p) e().sub_imm(a, hb, n * 4, a); else e().sub_imm(a, hb, n * 4 - 4, a); }
  e().and_imm(a, a, ~3u);
  u32 t_w = 0xFF;
  if (w && !(!u && p)) {                               // stmdb/ldmdb rn!: the writeback value is the start address
    t_w = cache_.temp();
    if (u) e().add_imm(t_w, hb, n * 4, t_w); else e().sub_imm(t_w, hb, n * 4, t_w);
  }
  cache_.unlock(hb);
  emit_block_transfer(instr, list, load, w, rn, a, t_w, pc_ + 12);
  cache_.release(a);
}

void Translator::thumb_ldr_str(u16 instr, TOp op) {
  Mem m;
  u32 rd;
  int const_nd = -1;
  u32 a;
  switch (op) {
  case TOp::LdrPcRel: {
    rd = (instr >> 8) & 7;
    const u32 addr = ((pc_ + 4) & ~3u) + ((instr & 0xFF) << 2);
    a = pc_const(addr);
    m = Mem::Ld32;
    if (a9_) { note_dep(addr, mem::Timing::RETIME_DATA); const_nd = cpu_.timing9[addr >> 12][2]; }
    break;
  }
  case TOp::LdrStrReg: {
    const u32 ro = (instr >> 6) & 7, rb = (instr >> 3) & 7;
    rd = instr & 7;
    const u32 hb = cache_.read(rb), ho = cache_.read(ro);
    a = cache_.temp();
    e().add_reg(a, hb, ho);
    cache_.unlock(hb); cache_.unlock(ho);
    static const Mem kinds[8] = {Mem::St32, Mem::St16, Mem::St8, Mem::Ld8S, Mem::Ld32, Mem::Ld16, Mem::Ld8, Mem::Ld16S};
    m = kinds[(instr >> 9) & 7];
    break;
  }
  case TOp::LdrStrImm5: {
    const u32 rb = (instr >> 3) & 7, imm = (instr >> 6) & 0x1F;
    rd = instr & 7;
    const bool l = instr & (1 << 11), b = instr & (1 << 12);
    const u32 hb = cache_.read(rb);
    a = cache_.temp();
    e().add_imm(a, hb, b ? imm : imm * 4, a);
    cache_.unlock(hb);
    m = b ? (l ? Mem::Ld8 : Mem::St8) : (l ? Mem::Ld32 : Mem::St32);
    break;
  }
  case TOp::LdrStrHImm5: {
    const u32 rb = (instr >> 3) & 7;
    rd = instr & 7;
    const u32 hb = cache_.read(rb);
    a = cache_.temp();
    e().add_imm(a, hb, ((instr >> 6) & 0x1F) * 2, a);
    cache_.unlock(hb);
    m = (instr & (1 << 11)) ? Mem::Ld16 : Mem::St16;
    break;
  }
  default: {   // LdrStrSpRel
    rd = (instr >> 8) & 7;
    a = cache_.temp();
    e().add_imm(a, pinned_host(13), (instr & 0xFF) * 4, a);
    m = (instr & (1 << 11)) ? Mem::Ld32 : Mem::St32;
    break;
  }
  }
  if (is_load(m)) emit_single(m, a, 0xFF, rd, true, const_nd);
  else emit_single(m, a, cache_.read(rd), 0, false);
  cache_.release(a);
}

void Translator::thumb_push_pop(u16 instr) {
  u32 list = instr & 0xFF;
  const bool pop = instr & (1 << 11), r = instr & (1 << 8);
  const u32 sp = pinned_host(13);
  const u32 a = cache_.temp();
  if (pop) {
    if (r) list |= 0x4000;   // lr (pc is a fallback)
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    e().mov(a, sp);
    const u32 t_w = cache_.temp();
    e().add_imm(t_w, sp, n * 4, t_w);                     // POP always writes back; sp is never in a Thumb list
    emit_block_transfer(instr, list, true, true, 13, a, t_w, 0);
  } else {
    if (r) list |= 0x4000;
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    e().sub_imm(a, sp, n * 4, a);
    emit_block_transfer(instr, list, false, true, 13, a, 0xFF, 0);   // writeback value = start address
  }
  cache_.release(a);
}

void Translator::thumb_stm_ldm(u16 instr) {
  const u32 rb = (instr >> 8) & 7, list = instr & 0xFF;
  const bool load = instr & (1 << 11);
  const u32 n = static_cast<u32>(__builtin_popcount(list));
  const u32 hb = cache_.read(rb);
  const u32 a = cache_.temp();
  e().mov(a, hb);
  const u32 t_w = cache_.temp();
  e().add_imm(t_w, hb, n * 4, t_w);
  cache_.unlock(hb);
  emit_block_transfer(instr, list, load, true, rb, a, t_w, 0);
  cache_.release(a);
}

// ---- ARM ---------------------------------------------------------------------------------------

// The guest word with its register fields substituted: shifter, carry-out and
// flags are the host's own. Operands are read before the destination is
// claimed so a fresh destination slot never evicts one of them.
void Translator::arm_data_processing(u32 instr, AOp op) {
  const u32 opcode = (instr >> 21) & 0xF;
  const bool s = instr & (1u << 20);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  const bool test = opcode >= 8 && opcode <= 0xB;
  const bool mov = opcode == 0xD || opcode == 0xF;
  if (op == AOp::DpRegShift) add_pending(numC_internal() + 1);
  else add_pending(numC(pc_));

  const u32 pcv = op == AOp::DpRegShift ? pc_ + 12 : pc_ + 8;
  u32 hn = 0, hm = 0, hs = 0;
  if (!mov) hn = reg_read(rn, pcv);
  if (op != AOp::DpImm) hm = reg_read(instr & 0xF, pcv);
  if (op == AOp::DpRegShift) hs = cache_.read((instr >> 8) & 0xF);
  const u32 hd = test ? 0 : cache_.write(rd, cond_ != AL);
  const DpOp dop = static_cast<DpOp>(opcode);
  const Shift sh = static_cast<Shift>((instr >> 5) & 3);
  flush_pending();   // before the body: a predicated charge after an S instruction would see the new flags
  if (op == AOp::DpImm) e().dp_imm(dop, s, hd, hn, instr & 0xFFF, cond_);
  else if (op == AOp::DpImmShift) e().dp_reg(dop, s, hd, hn, hm, sh, (instr >> 7) & 0x1F, cond_);
  else e().dp_regshift(dop, s, hd, hn, hm, sh, hs, cond_);
}

void Translator::arm_multiply(u32 instr, AOp op) {
  const bool s = instr & (1u << 20);
  const u32 rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  if (op == AOp::Mul || op == AOp::Mla) {
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF;
    if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(cache_.read(rs), true, op == AOp::Mla ? 1 : 0); }
    const u32 hm = cache_.read(rm), hs = cache_.read(rs);
    const u32 hn = op == AOp::Mla ? cache_.read(rn) : 0;
    const u32 hd = cache_.write(rd, cond_ != AL);
    flush_pending();
    if (op == AOp::Mla) e().mla(hd, hm, hs, hn, s, cond_); else e().mul(hd, hm, hs, s, cond_);
    if (s) emit_clear_c();
    return;
  }
  const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF;
  const bool sgn = op == AOp::Smull || op == AOp::Smlal;
  const bool acc = op == AOp::Umlal || op == AOp::Smlal;
  if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
  else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(cache_.read(rs), sgn, 1); }
  const u32 hm = cache_.read(rm), hs = cache_.read(rs);
  const bool keep = acc || cond_ != AL;
  const u32 hlo = cache_.write(rdlo, keep), hhi = cache_.write(rdhi, keep);
  flush_pending();
  if (acc) { if (sgn) e().smlal(hlo, hhi, hm, hs, s, cond_); else e().umlal(hlo, hhi, hm, hs, s, cond_); }
  else     { if (sgn) e().smull(hlo, hhi, hm, hs, s, cond_); else e().umull(hlo, hhi, hm, hs, s, cond_); }
  if (s) emit_clear_c();
}

// v5TE halfword multiplies, ARM9 only: native, Q included (the APSR carries
// the guest Q, convention.h). Cost is charge_C.
void Translator::arm_dsp_multiply(u32 instr, AOp op) {
  const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  const bool x = instr & (1u << 5), y = instr & (1u << 6);
  const bool acc = op == AOp::SmlaXY || op == AOp::SmlawY;
  add_pending(numC(pc_));
  const u32 hm = cache_.read(rm), hs = cache_.read(rs);
  const u32 hn = acc ? cache_.read(rn) : 0;
  const u32 hd = cache_.write(rd, cond_ != AL);
  flush_pending();
  switch (op) {
  case AOp::SmulXY: e().smul_xy(hd, hm, hs, x, y, cond_); break;
  case AOp::SmlaXY: e().smla_xy(hd, hm, hs, hn, x, y, cond_); break;
  case AOp::SmulwY: e().smulw_y(hd, hm, hs, y, cond_); break;
  default:          e().smlaw_y(hd, hm, hs, hn, y, cond_); break;
  }
}

void Translator::translate_arm(u32 instr, bool fb) {
  const u32 cond = instr >> 28;
  cond_ = AL;
  if (cond == 0xF) {
    if (((instr >> 25) & 7) == 5 && a9_) {       // BLX imm
      s32 off = static_cast<s32>(instr << 8) >> 6;
      off |= (instr >> 23) & 2;
      e().mov_imm(cache_.write(14, false), pc_ + 4);
      emit_branch_static((pc_ + 8 + static_cast<u32>(off)) & ~1u, true, true);
      return;
    }
    if (((instr >> 24) & 0xF7) == 0x55) { add_pending(numC(pc_)); return; }   // PLD
    emit_fallback(instr, true);
    return;
  }
  const AOp op = arm::decode_arm(instr);

  if (fb) {
    // The helper evaluates the condition itself. Jumps are possible for
    // PC-destination forms, exceptions and coprocessor/MSR side effects.
    bool always = false;
    switch (op) {
    case AOp::Swi: case AOp::Bkpt: case AOp::Undefined: case AOp::Cdp: case AOp::Ldc: case AOp::Stc:
    case AOp::Bx: case AOp::BlxReg:
      always = cond == 0xE; break;
    case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
      always = cond == 0xE && ((instr >> 12) & 0xF) == 15 && !(((instr >> 21) & 0xF) >= 8 && ((instr >> 21) & 0xF) <= 0xB);
      break;
    case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
      always = cond == 0xE && ((instr >> 12) & 0xF) == 15 && (instr & (1u << 20)) != 0;
      break;
    case AOp::Ldm:
      always = cond == 0xE && (instr & 0x8000) != 0;
      break;
    default: break;
    }
    emit_fallback(instr, always);
    return;
  }

  if (op == AOp::B || op == AOp::Bl) {
    const s32 off = static_cast<s32>(instr << 8) >> 6;
    auto taken = [&] {
      if (op == AOp::Bl) e().mov_imm(cache_.write(14, false), pc_ + 4);
      emit_branch_static(pc_ + 8 + static_cast<u32>(off), false, true);
    };
    if (cond != 0xE) emit_branch_cond(cond, taken); else taken();
    return;
  }

  // Memory: a data-dependent cost, so a conditional form branches around
  // the body (cache empty on both arms) and the skipped arm charges numC.
  switch (op) {
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: case AOp::Ldm: case AOp::Stm: {
    size_t skip = 0;
    if (cond != 0xE) {
      cache_.flush(); flush_pending();
      skip = hot_.b_fwd(invert(static_cast<Cond>(cond)));
      // The body's cold path is the interpreter, which evaluates the
      // condition again: the flags it reads must survive the page tests
      // even when nothing after the instruction reads them.
      live_ |= cond_reads(cond);
    }
    switch (op) {
    case AOp::LdrStrImm: case AOp::LdrStrReg: arm_ldr_str(instr, op); break;
    case AOp::LdrStrHImm: case AOp::LdrStrHReg: arm_ldr_str_h(instr, op); break;
    case AOp::Ldm: arm_ldm_stm(instr, true); break;
    default: arm_ldm_stm(instr, false); break;
    }
    if (cond != 0xE) {
      cache_.flush();
      flush_pending();
      const size_t join = hot_.b_fwd();
      hot_.bind(skip);
      add_pending(numC(pc_));
      flush_pending();
      hot_.bind(join);
    }
    return;
  }
  default: break;
  }

  // Everything below is a predicated body whose cost is numC plus static
  // or register-dependent extras: numC is charged before the condition,
  // the extras inside it and ahead of the body (an S body rewrites the
  // flags the predication reads). The one body that cannot be predicated
  // -- an ARM7 multiply that must clear C after setting N,Z -- is branched
  // around instead, with the cache empty on both arms.
  const bool conditional = cond != 0xE;
  bool branch_form = false;
  size_t skip = 0;
  if (conditional) {
    const u32 c = numC(pc_);
    add_pending(c);
    flush_pending();
    charged_ahead_ = c;
    switch (op) {
    case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
      branch_form = mul_clears_c(instr); break;
    default: break;
    }
    if (branch_form) { cache_.flush(); skip = hot_.b_fwd(invert(static_cast<Cond>(cond))); }
    else cond_ = static_cast<Cond>(cond);
  }

  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    arm_data_processing(instr, op);
    break;
  case AOp::Mrs: {
    const u32 rd = (instr >> 12) & 0xF;
    add_pending(numC(pc_));
    if (rd == 15) break;
    const u32 hd = cache_.write(rd, cond_ != AL);
    if (instr & (1u << 22)) { e().ldr(hd, R_CTX, OFF_SPSR, cond_); break; }
    const u32 t = cache_.temp();
    e().ldr(hd, R_CTX, OFF_CPSR, cond_);
    e().mrs_apsr(t, cond_);
    e().lsr_imm(t, t, 27, cond_);
    e().bfi(hd, t, 27, 5, cond_);
    cache_.release(t);
    break;
  }
  case AOp::Clz: {
    const u32 rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    add_pending(numC(pc_));
    if (rd == 15 || rm == 15) break;
    const u32 hm = cache_.read(rm);
    e().clz(cache_.write(rd, cond_ != AL), hm, cond_);
    break;
  }
  case AOp::Pld: add_pending(numC(pc_)); break;
  case AOp::Mcr: add_pending(numC(pc_) + 2); break;      // ignored cache operation: charge_CI(2)
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    arm_dsp_multiply(instr, op);
    break;
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    arm_multiply(instr, op);
    break;
  default: assert(false && "inline set and fallback set disagree"); break;
  }
  flush_pending();
  assert(charged_ahead_ == 0 && "a conditional instruction charged less than numC");
  if (branch_form) { cache_.flush(); hot_.bind(skip); }
  cond_ = AL;
}

// ---- Thumb ---------------------------------------------------------------------------------------

void Translator::thumb_alu(u16 instr) {
  const u32 rs = (instr >> 3) & 7, rd = instr & 7;
  const u32 aluop = (instr >> 6) & 0xF;
  switch (aluop) {
  case 0x2: case 0x3: case 0x4: case 0x7: {   // LSL LSR ASR ROR by register
    add_pending(numC_internal() + 1);
    const Shift type = aluop == 2 ? LSL : aluop == 3 ? LSR : aluop == 4 ? ASR : ROR;
    const u32 hs = cache_.read(rs), hd = cache_.write(rd, true);
    e().dp_regshift(MOV, true, hd, 0, hd, type, hs);
    return;
  }
  case 0xD: {   // MUL
    if (a9_) add_pending(numC(pc_) + 3);
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(cache_.read(rd), true, 0); }
    const u32 hs = cache_.read(rs), hd = cache_.write(rd, true);
    e().mul(hd, hd, hs, true);
    emit_clear_c();
    return;
  }
  default: break;
  }
  add_pending(numC(pc_));
  const u32 hs = cache_.read(rs);
  switch (aluop) {
  case 0x0: { const u32 hd = cache_.write(rd, true); e().dp_reg(AND, true, hd, hd, hs); break; }
  case 0x1: { const u32 hd = cache_.write(rd, true); e().dp_reg(EOR, true, hd, hd, hs); break; }
  case 0x5: { const u32 hd = cache_.write(rd, true); e().dp_reg(ADC, true, hd, hd, hs); break; }
  case 0x6: { const u32 hd = cache_.write(rd, true); e().dp_reg(SBC, true, hd, hd, hs); break; }
  case 0x8: e().tst_reg(cache_.read(rd), hs); break;
  case 0x9: { const u32 hd = cache_.write(rd, false); e().dp_imm(RSB, true, hd, hs, 0); break; }
  case 0xA: e().cmp_reg(cache_.read(rd), hs); break;
  case 0xB: e().dp_reg(CMN, true, 0, cache_.read(rd), hs); break;
  case 0xC: { const u32 hd = cache_.write(rd, true); e().dp_reg(ORR, true, hd, hd, hs); break; }
  case 0xE: { const u32 hd = cache_.write(rd, true); e().dp_reg(BIC, true, hd, hd, hs); break; }
  default:  { const u32 hd = cache_.write(rd, false); e().dp_reg(MVN, true, hd, 0, hs); break; }
  }
}

void Translator::translate_thumb(u16 instr, bool fb) {
  const TOp op = arm::decode_thumb(instr);
  const bool was_prefix = bl_prefix_valid_;
  bl_prefix_valid_ = false;
  cond_ = AL;
  if (fb) {
    bool always = false;
    switch (op) {
    case TOp::Swi: case TOp::Bkpt: case TOp::Undefined: case TOp::BxBlx: case TOp::BlSuffix: case TOp::BlxSuffix:
      always = true; break;
    case TOp::HiRegOp: always = true; break;          // only the pc-destination forms reach here
    case TOp::PushPop: always = (instr & (1 << 11)) && (instr & (1 << 8)); break;
    case TOp::StmLdm: always = false; break;
    default: break;
    }
    emit_fallback(instr, always);
    return;
  }
  switch (op) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    const u32 hs = cache_.read(rs), hd = cache_.write(rd, false);
    e().dp_reg(MOV, true, hd, 0, hs, static_cast<Shift>(type), amt);   // LSR/ASR #0 encode #32, as the guest
    return;
  }
  case TOp::AddSubReg: {
    const u32 rn = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    const u32 hs = cache_.read(rs), hn = cache_.read(rn), hd = cache_.write(rd, false);
    e().dp_reg((instr & (1 << 9)) ? SUB : ADD, true, hd, hs, hn);
    return;
  }
  case TOp::AddSubImm3: {
    const u32 imm = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    const u32 hs = cache_.read(rs), hd = cache_.write(rd, false);
    e().dp_imm((instr & (1 << 9)) ? SUB : ADD, true, hd, hs, imm);
    return;
  }
  case TOp::MovCmpAddSubImm8: {
    const u32 rd = (instr >> 8) & 7, imm = instr & 0xFF;
    add_pending(numC(pc_));
    switch ((instr >> 11) & 3) {
    case 0: e().dp_imm(MOV, true, cache_.write(rd, false), 0, imm); break;
    case 1: e().dp_imm(CMP, true, 0, cache_.read(rd), imm); break;
    case 2: { const u32 hd = cache_.write(rd, true); e().dp_imm(ADD, true, hd, hd, imm); break; }
    default: { const u32 hd = cache_.write(rd, true); e().dp_imm(SUB, true, hd, hd, imm); break; }
    }
    return;
  }
  case TOp::Alu: thumb_alu(instr); return;
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), rs = (instr >> 3) & 0xF;
    add_pending(numC(pc_));
    switch ((instr >> 8) & 3) {
    case 0: { const u32 b = reg_read(rs, pc_ + 4), hd = cache_.write(rd, true); e().add_reg(hd, hd, b); return; }
    case 1: { const u32 a = reg_read(rd, pc_ + 4), b = reg_read(rs, pc_ + 4); e().cmp_reg(a, b); return; }
    case 2: { const u32 b = reg_read(rs, pc_ + 4), hd = cache_.write(rd, false); e().mov(hd, b); return; }
    default: return;
    }
  }
  case TOp::AddPcSp: {
    const u32 rd = (instr >> 8) & 7, imm = (instr & 0xFF) * 4;
    add_pending(numC(pc_));
    const u32 hd = cache_.write(rd, false);
    if (instr & (1 << 11)) e().add_imm(hd, pinned_host(13), imm, hd);
    else e().mov_imm(hd, ((pc_ + 4) & ~3u) + imm);
    return;
  }
  case TOp::AdjustSp: {
    const u32 imm = (instr & 0x7F) * 4, sp = pinned_host(13);
    add_pending(numC(pc_));
    if (instr & (1 << 7)) e().sub_imm(sp, sp, imm, SCRATCH0); else e().add_imm(sp, sp, imm, SCRATCH0);
    return;
  }
  case TOp::BCond: {
    const u32 cond = (instr >> 8) & 0xF;
    const s32 off = static_cast<s8>(instr & 0xFF) * 2;
    emit_branch_cond(cond, [&] { emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true); });
    return;
  }
  case TOp::B: {
    const s32 off = static_cast<s32>(static_cast<u32>(instr) << 21) >> 20;
    emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true);
    return;
  }
  case TOp::BlPrefix: {
    const s32 off = static_cast<s32>(static_cast<u32>(instr) << 21) >> 9;
    add_pending(numC(pc_));
    bl_prefix_lr_ = pc_ + 4 + static_cast<u32>(off);
    e().mov_imm(cache_.write(14, false), bl_prefix_lr_);
    bl_prefix_valid_ = true;
    return;
  }
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel:
    thumb_ldr_str(instr, op);
    return;
  case TOp::PushPop: thumb_push_pop(instr); return;
  case TOp::StmLdm: thumb_stm_ldm(instr); return;
  case TOp::BlSuffix: case TOp::BlxSuffix: {
    assert(was_prefix); (void)was_prefix;
    const bool blx = op == TOp::BlxSuffix;
    const u32 target = bl_prefix_lr_ + ((instr & 0x7FF) << 1);
    e().mov_imm(cache_.write(14, false), (pc_ + 2) | 1);
    if (blx) emit_branch_static(target & ~3u, false, true);
    else emit_branch_static(target & ~1u, true, true);
    return;
  }
  default:
    assert(false && "inline set and fallback set disagree");
    return;
  }
}

// ---- block driver ------------------------------------------------------------------------------------

bool Translator::run() {
  const u32 start = key_pc(key_);
  if (!a9_) { t7_ = cpu_.timing7[start >> 15]; code_region7_ = start >> 24; }
  blk_.ndep = 0;
  blk_.dep_overflow = rt().cpu_oc;   // main RAM's costs baked into every access: dies on any retime

  // Decode the straight-line run.
  u32 addr = start;
  for (u32 i = 0; i < MAX_INSTRS; ++i) {
    const u32 raw = fetch(addr);
    const bool paired = thumb_ && i > 0 && arm::decode_thumb(static_cast<u16>(instrs_.back().raw)) == TOp::BlPrefix;
    const bool fb = (thumb_ ? thumb_needs_fallback(static_cast<u16>(raw), a9_, paired) : arm_needs_fallback(raw, a9_)) || needs_fallback_cost7(raw);
    instrs_.push_back({addr, raw, F_ALL, fb});
    const bool ends = thumb_ ? shape::thumb_ends_block(static_cast<u16>(raw)) : shape::arm_ends_block(raw, a9_);
    addr += step();
    if (ends) break;
    if (!cpu_.page_table.read_ptr(addr)) break;   // do not walk into unmapped space
  }

  // Backward flag liveness: which flags each instruction leaves live, and
  // whether the block reads flags before writing them (the entry test then
  // keeps them across its `tst`).
  u32 live = F_ALL;
  for (size_t i = instrs_.size(); i-- > 0;) {
    instrs_[i].live_out = live;
    const bool paired = thumb_ && i > 0 && arm::decode_thumb(static_cast<u16>(instrs_[i - 1].raw)) == TOp::BlPrefix;
    const FlagUse u = instrs_[i].fallback ? FlagUse{F_ALL, 0}
                    : thumb_ ? thumb_flag_use(static_cast<u16>(instrs_[i].raw), a9_, paired) : arm_flag_use(instrs_[i].raw, a9_);
    live = u.reads | (live & ~u.writes);
  }

  emit_budget_check(key_, live != 0);

  u32 end_addr = addr;
  for (size_t i = 0; i < instrs_.size(); ++i) {
    const Instr& in = instrs_[i];
    if (i > 0 && hot_.size() + cold_.size() > BLOCK_LIMIT) { end_addr = in.addr; break; }
    if (hot_.remaining() < 8192 || cold_.remaining() < 4096) return false;
    pc_ = in.addr;
    live_ = in.live_out;
    cache_.begin_instr();
    if (rt().trace) emit_call2(reinterpret_cast<const void*>(&jit_h_trace), in.raw, make_key(in.addr, thumb_));
    if (rt().cyclog) emit_call2(reinterpret_cast<const void*>(&jit_h_cyclog), in.raw, make_key(in.addr, thumb_));
    if (thumb_) translate_thumb(static_cast<u16>(in.raw), in.fallback); else translate_arm(in.raw, in.fallback);
    rt().stats.instrs_translated++;
    if (ended_) break;
    if (rt().strict) {
      // The interpreter tests the budget before every instruction; reproduce
      // that so the two engines interleave identically (verification mode).
      cache_.flush();
      flush_pending();
      emit_budget_check(make_key(in.addr + step(), thumb_), in.live_out != 0);
    }
  }
  blk_.guest_len = end_addr - start;
  if (!ended_) emit_branch_static(end_addr, thumb_, false);
  return finish();
}

} // namespace

bool backend::translate_block(JitCpu& jc, u32 key, u8* buf, size_t cap, Block& b, u32& size) {
  Emitter e(buf, cap);
  Translator t(jc, key, e, b);
  if (!t.run()) return false;
  size = static_cast<u32>(e.size());
  if (rt().debug) {
    std::fprintf(stderr, "[jit] block %08x:", key);
    for (u32 i = 0; i < size; i += 4) { u32 w; std::memcpy(&w, buf + i, 4); std::fprintf(stderr, " %08x", w); }
    std::fprintf(stderr, "\n");
  }
  return true;
}

} // namespace ds::jit
