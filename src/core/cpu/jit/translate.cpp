// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One-pass block translator, ARM and Thumb. Semantics follow the interpreter
// (interp_arm.cpp / interp_thumb.cpp) instruction for instruction, including
// its cycle model (cpu_cycles.h). An instruction that is not translated inline
// runs through the fallback stub, which *is* the interpreter, so every block
// is complete and exact from the first build; inlining is an optimisation
// that the interpreter-vs-JIT differential test guards.
//
// Layout of a block: the hot section holds the fast paths in guest order;
// everything reached only on a rare condition (budget exhausted, page-table
// miss, interpreter fallback of a block transfer) goes to a cold section that
// is appended after the hot code when the block is finished, so the hot code
// of a block is contiguous and the I-cache holds only what runs. Cold code is
// written to a side buffer and spliced in; branches between the sections
// and from the cold section to the stubs are recorded as fixups and resolved
// at the splice.
//
// Scratch register use inside one instruction (see jit_internal.h):
//   w0   loaded value / helper result / branch target
//   w1   effective address (preserved across the cost arithmetic and the slow path)
//   x2,x3 page-table entry / host base
//   w4-w7 temporaries of the shifter, the cost arithmetic, flag merging;
//        w7 = writeback value (preserved by the slow path)
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu_cycles.h"
#include "core/nds.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>

namespace ds::jit {

namespace {

using arm::AOp;
using arm::TOp;

constexpr u32 MAX_INSTRS = 64;
constexpr size_t BLOCK_LIMIT = 28u << 10;    // hot + cold bytes; keeps every cross-section tbnz in range
constexpr size_t COLD_CAP = 32u << 10;
constexpr u32 OFF_SPSR = offsetof(CpuContext, hot) + offsetof(JitHot, spsr);

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

// Instructions the translator hands to the interpreter whole. The liveness
// pass treats these as reading every flag (the helper syncs CPSR and may
// observe it: exceptions, MSR, MRS...), so the two must agree.
// CP15 writes the core accepts and ignores (cp15.cpp): every c7 cache /
// write-buffer operation except wait-for-interrupt (c7,c0,4 and c7,c8,2).
bool mcr_is_nop(u32 instr, bool a9) {
  if (!a9 || ((instr >> 8) & 0xF) != 15) return false;
  const u32 crn = (instr >> 16) & 0xF, crm = instr & 0xF, opc2 = (instr >> 5) & 7;
  return crn == 7 && !((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2));
}
// MSR forms translated inline: CPSR writes from a register or immediate. The
// mode must not change (tested at run time; otherwise the interpreter runs it).
bool msr_inline(u32 instr) {
  if (instr & (1u << 22)) return false;                                   // SPSR
  return !(arm::decode_arm(instr) == AOp::MsrReg && (instr & 0xF) == 15);
}

bool arm_needs_fallback(u32 instr, bool a9) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) return !(((instr >> 25) & 7) == 5 && a9) && ((instr >> 24) & 0xF7) != 0x55;   // BLX imm (ARM9) and PLD inline
  const AOp op = arm::decode_arm(instr);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool test = opcode >= 8 && opcode <= 0xB;
    return rd == 15 && !test;
  }
  case AOp::Mrs: case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::Pld:
    return false;
  case AOp::Mcr: return !mcr_is_nop(instr, a9);
  case AOp::MsrReg: case AOp::MsrImm: return !msr_inline(instr);
  case AOp::BlxReg: case AOp::Clz:
    return !a9;
  case AOp::Mul: case AOp::Mla:
    return rd == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 || rn == 15;
  case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    return rd == 15 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15;
  // v5TE DSP multiplies: ARM9 only (undefined on the ARM7). Destination is
  // bits 19:16 and the accumulate operand bits 15:12 -- the opposite of the
  // data-processing layout `rd`/`rn` above.
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
    return (instr & (1u << 22)) || (instr & 0xFFFF) == 0 || rn == 15;
  default:
    return true;
  }
}

bool thumb_needs_fallback(u16 instr, bool a9) {
  switch (arm::decode_thumb(instr)) {
  case TOp::BxBlx: return (instr & (1 << 7)) && !a9;
  case TOp::BlxSuffix: return !a9;
  case TOp::PushPop: return ((instr & 0xFF) | ((instr >> 8) & 1)) == 0;
  case TOp::StmLdm: return (instr & 0xFF) == 0;
  case TOp::Swi: case TOp::Bkpt: case TOp::Undefined: return true;
  default: return false;
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
  const u32 rd = (instr >> 12) & 0xF;
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    if (rd == 15 && s) { u = {F_ALL, F_ALL}; break; }
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
    if (s) u.writes = F_N | F_Z;
    break;
  case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Clz: case AOp::Pld:
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
  case AOp::Ldm: case AOp::Stm:
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    break;
  case AOp::Mrs: u.reads = F_ALL; break;
  case AOp::Mcr: break;
  case AOp::MsrReg: case AOp::MsrImm:
    u.reads = F_ALL;                                   // the mode-change path syncs CPSR through the interpreter
    if (instr & (1u << 19)) u.writes = F_ALL;          // f field
    break;
  default: u = {F_ALL, F_ALL}; break;
  }
  if (cond != 0xE) { u.reads |= cond_reads(cond); u.writes = 0; }
  return u;
}

FlagUse thumb_flag_use(u16 instr, bool a9) {
  if (thumb_needs_fallback(instr, a9)) return {F_ALL, 0};
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
  case TOp::Swi: case TOp::Bkpt: case TOp::Undefined: return {F_ALL, F_ALL};
  default: return {0, 0};
  }
}

enum class CarryKind { Keep, Const, Reg };
struct Carry { CarryKind kind = CarryKind::Keep; u32 value = 0; u32 reg = 0; };

// An operand: a host register or a constant (the pc-relative cases).
struct Operand { bool imm; u32 reg; u32 value; };

struct Instr { u32 addr; u32 raw; u32 live_out; };

inline u32 rotr(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

class Translator {
public:
  // The cold-section scratch is reused across translations: a fresh
  // std::vector<u8>(COLD_CAP) here was a 32 KB allocation AND memset per
  // block -- ~600 MB of memset across an overlay-heavy scene, a third of a
  // burst frame's translate stall. The Emitter tracks its own size, so stale
  // bytes past it are never read. thread_local for the pre-translation worker.
  static u8* cold_scratch() {
    static thread_local std::unique_ptr<u8[]> buf;
    if (!buf) buf.reset(new u8[COLD_CAP]);
    return buf.get();
  }
  Translator(JitCpu& jc, u32 key, Emitter& e, Block& b)
      : jc_(jc), cpu_(*jc.ctx), hot_(e), blk_(b), key_(key), thumb_(key_thumb(key)), a9_(jc.arm9),
        cold_buf_(cold_scratch()), cold_(cold_buf_, COLD_CAP), cur_(&hot_) {}

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
  u32 pending_ = 0;          // static cycles not yet subtracted from the budget
  u32 charged_ahead_ = 0;    // cycles already charged before a conditional instruction's body
  bool ended_ = false;
  bool dp_csel_ = false;   // arm_data_processing: write SCRATCH6, not the guest register
  const u8* t7_ = nullptr;
  u32 code_region7_ = 0;
  // Thumb BL pairing: static lr value after a BL prefix at the previous address.
  bool bl_prefix_valid_ = false;
  u32  bl_prefix_lr_ = 0;

  // ---- hot / cold sections ------------------------------------------------------------
  // DS_JIT_DENSITY
  DensitySlot* dslot_ = nullptr;
  u32 dinstrs_ = 0;            // guest instructions translated inline into this block
  u32 dbytes_ = 0;             // bytes the counter code itself added to the hot section

  u8* cold_buf_;               // thread-reused scratch (cold_scratch), COLD_CAP bytes
  Emitter cold_;
  Emitter* cur_;
  struct Fix { size_t at; bool at_cold; size_t target; bool target_cold; const void* abs; };
  std::vector<Fix> fixes_;

  Emitter& e() { return *cur_; }
  bool in_cold() const { return cur_ == &cold_; }
  u32 step() const { return thumb_ ? 2 : 4; }

  // `bl stub` / `b stub` from the current section.
  void call_stub(const u8* stub) {
    if (!in_cold()) { hot_.bl(stub); return; }
    fixes_.push_back({cold_.bl_fwd(), true, 0, false, stub});
  }
  void jump_stub(const u8* stub) {
    if (!in_cold()) { hot_.b(stub); return; }
    fixes_.push_back({cold_.b_fwd(), true, 0, false, stub});
  }
  // Start a cold path reached by the given hot forward branches.
  void cold_begin(const std::vector<size_t>& hot_fixups) {
    assert(!in_cold());
    for (size_t f : hot_fixups) fixes_.push_back({f, false, cold_.size(), true, nullptr});
    cur_ = &cold_;
  }
  void cold_end() { cur_ = &hot_; }
  // End a cold path with a jump back to `hot_target`.
  void cold_end_jump(size_t hot_target) {
    fixes_.push_back({cold_.b_fwd(), true, hot_target, false, nullptr});
    cur_ = &hot_;
  }
  // Bump this translation's execution counter. Emitted at the block entry,
  // ahead of the budget check, so it counts entries however the block was
  // reached -- dispatcher, LUT probe or a linked branch. x0/x1 are scratch at
  // a block boundary (enter_light has already reloaded the guest registers and
  // flags and left through `br x1`), and none of these forms writes NZCV, so a
  // block entered by a linked branch keeps the guest flags the branch left.
  void emit_density_bump() {
    dslot_ = density_new_slot();
    if (!dslot_) return;
    const size_t before = hot_.size();
    hot_.mov_imm64(SCRATCH0, reinterpret_cast<u64>(&dslot_->execs));
    hot_.ldr_x(SCRATCH1, SCRATCH0, 0);
    hot_.add_imm(SCRATCH1, SCRATCH1, 1, true, false);
    hot_.str_x(SCRATCH1, SCRATCH0, 0);
    dbytes_ = static_cast<u32>(hot_.size() - before);
  }

  // Splice the cold section after the hot code and resolve every fixup.
  bool finish() {
    assert(!in_cold());
    const size_t cold_base = hot_.size();
    blk_.hot_size = static_cast<u32>(cold_base);
    if (dslot_) { dslot_->hot_bytes = blk_.hot_size - dbytes_; dslot_->guest_instrs = dinstrs_; }
    if (hot_.remaining() < cold_.size() + 64) return false;
    std::memcpy(hot_.cur(), cold_buf_, cold_.size());
    hot_.set_pos(cold_base + cold_.size());
    u8* base = hot_.base();
    for (const Fix& f : fixes_) {
      u8* at = base + (f.at_cold ? cold_base + f.at : f.at);
      const u8* target = f.abs ? static_cast<const u8*>(f.abs) : base + (f.target_cold ? cold_base + f.target : f.target);
      Emitter::patch_rel(at, target);
    }
    assert(hot_.size() >= 12 && "kill_block patches 12 bytes at the entry");
    return true;
  }

  // ---- decoding ------------------------------------------------------------------------
  u32 fetch(u32 addr) {
    if (u8* p = cpu_.page_table.read_ptr(addr)) {
      if (thumb_) { u16 v; std::memcpy(&v, p, 2); return v; }
      u32 v; std::memcpy(&v, p, 4); return v;
    }
    return thumb_ ? cpu_.nds->bus.read16(cpu_.which, addr) : cpu_.nds->bus.read32(cpu_.which, addr);
  }
  static bool arm_ends_block(u32 instr, bool a9) {
    const u32 cond = instr >> 28;
    if (cond == 0xF) return true;
    switch (arm::decode_arm(instr)) {
    case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Swi: case AOp::Bkpt: case AOp::Undefined:
    case AOp::Cdp: case AOp::Ldc: case AOp::Stc: case AOp::Mrc:
      return true;
    case AOp::Mcr: return !mcr_is_nop(instr, a9);
    case AOp::MsrReg: case AOp::MsrImm: return !msr_inline(instr);
    case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
      const u32 opcode = (instr >> 21) & 0xF;
      const bool test = opcode >= 8 && opcode <= 0xB;
      return !test && ((instr >> 12) & 0xF) == 15;
    }
    case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
      return (instr & (1u << 20)) && ((instr >> 12) & 0xF) == 15;
    case AOp::Ldm: return (instr & 0x8000) != 0;
    default: return false;
    }
  }
  static bool thumb_ends_block(u16 instr) {
    switch (arm::decode_thumb(instr)) {
    case TOp::HiRegOp: {
      const u32 rd = (instr & 7) | ((instr >> 4) & 8), op = (instr >> 8) & 3;
      return rd == 15 && (op == 0 || op == 2);
    }
    case TOp::BxBlx: case TOp::BCond: case TOp::Swi: case TOp::B: case TOp::BlSuffix: case TOp::BlxSuffix:
    case TOp::Bkpt: case TOp::Undefined:
      return true;
    case TOp::PushPop: return (instr & (1 << 11)) && (instr & (1 << 8));
    case TOp::StmLdm: return (instr & 0xFF) == 0;
    default: return false;
    }
  }

  // ---- cycles -----------------------------------------------------------------------------
  // Record that this translation baked byte `kind` (mem::Timing::RETIME_*) of
  // the ARM9 entry for `addr`'s 4 KB page: the retime path kills the block
  // only when one of these bytes changes (Block::dep_*). Blocks are at most
  // 64 instructions, so the set is small; a fuller one dies conservatively.
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
  // The whole CD/CDI charge for a data cost known at translate time (a
  // pc-relative literal, or every access under --cpu-oc). Mirrors
  // emit_charge_data_body exactly; the ARM7 arm takes the data as main RAM,
  // which only --cpu-oc reaches.
  u32 const_charge(u32 nd, bool cdi) const {
    const s32 d = static_cast<s32>(nd);
    if (rt().fastcost) return (a9_ ? numC(pc_) : numC_nonseq7()) + nd;   // DS_JIT_FASTCOST's inexact numC + numD
    if (a9_) { const s32 nc = static_cast<s32>(numC(pc_)); return max3(nc + d - 6, nc, d); }
    const s32 nc = static_cast<s32>(numC_nonseq7());
    if (code_region7_ == 0x02) return static_cast<u32>(d + nc);
    const s32 ncx = nc + (cdi ? 1 : 0);
    return max3(ncx, d, d + ncx - 3);
  }
  // --cpu-oc: the data cost of a main-RAM access of this width, from the
  // tables as they stand at translate time (see Runtime::cpu_oc).
  u32 oc_data_cost(bool word, bool seq, bool store) const {
    if (a9_) return cpu_.timing9[0x02000000u >> 12][(store ? 4 : 0) + (seq ? 3 : (word ? 2 : 1))];
    return cpu_.timing7[0x02000000u >> 15][seq ? (word ? 3 : 1) : (word ? 2 : 0)];
  }
  u32 numC_nonseq7() const { return t7_[thumb_ ? 0 : 2]; }
  u32 numC_internal() const { return a9_ ? numC(pc_) : numC_nonseq7(); }   // base cost of a CI instruction
  void add_pending(u32 c) {
    if (charged_ahead_) { const u32 k = std::min(c, charged_ahead_); c -= k; charged_ahead_ -= k; }
    pending_ += c;
  }
  void flush_pending() {
    if (!pending_) return;
    e().sub_imm_any(R_BUDGET, R_BUDGET, pending_, SCRATCH0);
    pending_ = 0;
  }
  // Leave with `exit_key` when the budget is exhausted (hot: one tbnz; the exit is cold).
  void emit_budget_check(u32 exit_key) {
    assert(!in_cold());
    const size_t t = hot_.tbnz_fwd(R_BUDGET, 31);
    cold_begin({t});
    call_stub(rt().exit_key_lit);
    e().word(exit_key);
    cold_end();
  }

  // ---- flags ---------------------------------------------------------------------------------
  // N,Z from `res` (already computed by a flag-neutral op), C per `carry`, V
  // kept. Only live flags are reproduced. Temporaries: x0-x3.
  void set_flags_logical(u32 res, const Carry& carry, bool res64 = false) {
    const bool need_c = live_ & F_C, need_v = live_ & F_V;
    if (!need_c && !need_v) { e().tst_reg(res, res, res64); return; }
    const bool keep_c = need_c && carry.kind == CarryKind::Keep;
    if (res64) {   // long multiplies: inline merge
      if (keep_c || need_v) e().mrs_nzcv(SCRATCH1);
      e().tst_reg(res, res, true);
      e().mrs_nzcv(SCRATCH2);
      if (keep_c && need_v) { e().ubfx(SCRATCH3, SCRATCH1, 28, 2, true); e().bfi(SCRATCH2, SCRATCH3, 28, 2, true); }
      else {
        if (need_v) { e().ubfx(SCRATCH3, SCRATCH1, 28, 1, true); e().bfi(SCRATCH2, SCRATCH3, 28, 1, true); }
        if (keep_c) { e().ubfx(SCRATCH3, SCRATCH1, 29, 1, true); e().bfi(SCRATCH2, SCRATCH3, 29, 1, true); }
      }
      if (need_c && carry.kind == CarryKind::Const && carry.value) e().orr_imm(SCRATCH2, SCRATCH2, 1u << 29);
      if (need_c && carry.kind == CarryKind::Reg) e().bfi(SCRATCH2, carry.reg, 29, 1, true);
      e().msr_nzcv(SCRATCH2);
      return;
    }
    assert(res != SCRATCH1);
    if (!need_c || keep_c) {   // C kept (or dead), V kept
      if (res != SCRATCH0) e().mov(SCRATCH0, res);
      call_stub(rt().merge_keep_cv);
      return;
    }
    if (!need_v) {             // C set, V dead: inline
      e().tst_reg(res, res);
      e().mrs_nzcv(SCRATCH2);
      if (carry.kind == CarryKind::Const) { if (carry.value) e().orr_imm(SCRATCH2, SCRATCH2, 1u << 29); }
      else e().bfi(SCRATCH2, carry.reg, 29, 1, true);
      e().msr_nzcv(SCRATCH2);
      return;
    }
    assert(carry.kind != CarryKind::Reg || carry.reg != SCRATCH0);
    if (carry.kind == CarryKind::Const) e().movz(SCRATCH1, carry.value);
    else if (carry.reg != SCRATCH1) e().mov(SCRATCH1, carry.reg);
    if (res != SCRATCH0) e().mov(SCRATCH0, res);
    call_stub(rt().merge_set_c);
  }

  // ---- operands ---------------------------------------------------------------------------------
  Operand reg_operand(u32 r, u32 pc_value) {
    if (r == 15) return {true, 0, pc_value};
    return {false, host_reg(r), 0};
  }
  u32 to_reg(const Operand& o, u32 scratch) {
    if (!o.imm) return o.reg;
    e().mov_imm(scratch, o.value);
    return scratch;
  }

  // Immediate-amount shifter. Result in `scratch` (or the source register /
  // a constant); carry-out in `cscratch` when wanted. Uses x7 for RRX.
  Operand shift_imm(u32 type, u32 amt, const Operand& rm, bool want_carry, Carry& carry, u32 scratch, u32 cscratch) {
    carry = {};
    if (rm.imm && !(type == 3 && amt == 0)) {
      bool c = false; u32 v = rm.value;
      switch (type) {
      case 0: if (amt == 0) return rm; c = (v >> (32 - amt)) & 1; v <<= amt; break;
      case 1: if (amt == 0) { c = v >> 31; v = 0; } else { c = (v >> (amt - 1)) & 1; v >>= amt; } break;
      case 2: if (amt == 0) { c = v >> 31; v = static_cast<u32>(static_cast<s32>(v) >> 31); } else { c = (v >> (amt - 1)) & 1; v = static_cast<u32>(static_cast<s32>(v) >> amt); } break;
      default: c = (v >> (amt - 1)) & 1; v = rotr(v, amt); break;
      }
      carry = {CarryKind::Const, c, 0};
      return {true, 0, v};
    }
    const u32 m = to_reg(rm, scratch);
    switch (type) {
    case 0:
      if (amt == 0) return {false, m, 0};
      if (want_carry) { e().ubfx(cscratch, m, 32 - amt, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().lsl_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 1:
      if (amt == 0) { if (want_carry) { e().lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } return {true, 0, 0}; }
      if (want_carry) { e().ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().lsr_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 2:
      if (amt == 0) { if (want_carry) { e().lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } e().asr_imm(scratch, m, 31); return {false, scratch, 0}; }
      if (want_carry) { e().ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e().asr_imm(scratch, m, amt);
      return {false, scratch, 0};
    default:
      if (amt == 0) {   // RRX
        if (want_carry) { e().and_imm(cscratch, m, 1); carry = {CarryKind::Reg, 0, cscratch}; }
        e().cset(SCRATCH7, CS);
        e().extr(scratch, SCRATCH7, m, 1);
        return {false, scratch, 0};
      }
      e().ror_imm(scratch, m, amt);
      if (want_carry) { e().lsr_imm(cscratch, scratch, 31); carry = {CarryKind::Reg, 0, cscratch}; }
      return {false, scratch, 0};
    }
  }

  // Register-amount shifter (amount = rs & 0xFF, ARM rules for >= 32).
  // Result in `scratch`, carry in `cscratch`; uses x4-x7.
  Operand shift_reg(u32 type, u32 rs_host, const Operand& rm, bool want_carry, Carry& carry, u32 scratch, u32 cscratch) {
    carry = {};
    const u32 m = to_reg(rm, scratch);
    const u32 amt = SCRATCH4, tmp = SCRATCH5, old_c = SCRATCH6, wide = SCRATCH7;
    e().and_imm(amt, rs_host, 0xFF);
    if (want_carry) e().cset(old_c, CS);
    if (type == 3) {
      e().rorv(scratch, m, amt);
      if (want_carry) {
        e().lsr_imm(cscratch, scratch, 31);
        size_t nz = e().cbnz_fwd(amt);
        e().mov(cscratch, old_c);
        e().bind(nz);
        carry = {CarryKind::Reg, 0, cscratch};
      }
      return {false, scratch, 0};
    }
    // amt' = min(amt, 33): 32 keeps its own carry rule, everything above
    // behaves like 33, and a 64-bit shift by amt' then yields result and
    // carry exactly (tmp = amt - max(amt - 33, 0)).
    e().sub_imm(tmp, amt, 33);
    e().bic_reg(tmp, tmp, tmp, ASR, 31);
    e().sub_reg(tmp, amt, tmp);
    switch (type) {
    case 0:
      e().mov(wide, m);
      e().lslv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 32, 1, true);
      e().mov(scratch, wide);
      break;
    case 1:
      e().lsl_imm(wide, m, 32, true);
      e().lsrv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 31, 1, true);
      e().lsr_imm(scratch, wide, 32, true);
      break;
    default:
      e().sbfm(wide, m, 0, 31, true);
      e().lsl_imm(wide, wide, 32, true);
      e().asrv(wide, wide, tmp, true);
      if (want_carry) e().ubfx(cscratch, wide, 31, 1, true);
      e().lsr_imm(scratch, wide, 32, true);
      break;
    }
    if (want_carry) {
      size_t nz = e().cbnz_fwd(amt);
      e().mov(cscratch, old_c);
      e().bind(nz);
      carry = {CarryKind::Reg, 0, cscratch};
    }
    return {false, scratch, 0};
  }

  // ---- helpers: fallback, trace ------------------------------------------------------------------
  // Run this instruction through the interpreter (it evaluates the condition
  // itself and charges its own cycles). The stub polls, returns when the
  // instruction did not jump and dispatches when it did.
  void emit_fallback(u32 instr, bool always_jumps) {
    flush_pending();
    call_stub(jc_.fallback);
    e().word(instr);
    e().word(make_key(pc_, thumb_));
    if (always_jumps) ended_ = true;
  }
  void emit_call2(const void* fn, u32 instr, u32 key) {
    call_stub(rt().call2);
    e().word(instr);
    e().word(key);
    const u64 f = reinterpret_cast<u64>(fn);
    e().word(static_cast<u32>(f));
    e().word(static_cast<u32>(f >> 32));
  }
  void emit_trace(u32 instr) { emit_call2(reinterpret_cast<const void*>(&jit_h_trace), instr, make_key(pc_, thumb_)); }

  // ---- branches ----------------------------------------------------------------------------------
  void emit_branch_static(u32 target, bool to_thumb, bool refill) {
    if (refill) {
      if (a9_) {   // the pages refill_cycles reads (cpu_cycles.h)
        if (!to_thumb) { note_dep(target, mem::Timing::RETIME_CODE); note_dep(target + 4, mem::Timing::RETIME_CODE); }
        else if (target & 2) { note_dep(target - 2, mem::Timing::RETIME_CODE); note_dep(target + 2, mem::Timing::RETIME_CODE); }
        else note_dep(target, mem::Timing::RETIME_CODE);
      }
      add_pending(refill_cycles(cpu_, target, to_thumb));
    }
    flush_pending();
    if (to_thumb != thumb_) {
      e().ldr_w(SCRATCH0, R_CTX, OFF_CPSR);
      e().eor_imm(SCRATCH0, SCRATCH0, 0x20);
      e().str_w(SCRATCH0, R_CTX, OFF_CPSR);
    }
    call_stub(jc_.link);
    e().word(make_key(target, to_thumb));
    if (blk_.nsucc < 4) blk_.succ[blk_.nsucc++] = make_key(target, to_thumb);
    ended_ = true;
  }
  // Indirect branch; `wtarget` holds the address. With `interwork` bit 0
  // selects the state, otherwise the current state is kept.
  void emit_branch_indirect(u32 wtarget, bool interwork, bool cdi = false) {
    flush_pending();
    if (wtarget != SCRATCH0) e().mov(SCRATCH0, wtarget);
    if (!interwork) {
      if (thumb_) e().orr_imm(SCRATCH0, SCRATCH0, 1);
      else e().and_imm(SCRATCH0, SCRATCH0, ~1u);
    }
    jump_stub(cdi ? jc_.branch_indirect_cdi : jc_.branch_indirect);
    ended_ = true;
  }

  // ---- memory ---------------------------------------------------------------------------------------
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

  // DS_JIT_MEMPROBE: see the field's comment in jit_internal.h. The duplicate
  // writes only SCRATCH2/SCRATCH3, both of which the real walk overwrites
  // before anything reads them.
  bool memprobe_on() const {
    const int c = rt().memprobe;
    return c == 1 || (c == 9 && a9_) || (c == 7 && !a9_);
  }
  void emit_walk_probe(u32 waddr) {
    if (!memprobe_on()) return;
    e().lsr_imm(SCRATCH2, waddr, mem::PAGE_SHIFT);
    e().ldr_x_reg(SCRATCH2, R_PT, SCRATCH2, true, true);
    e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
  }

  // Page-table lookup for `waddr` (hot). On success x3 = pre-biased host
  // base. Failure branches are collected in `fail`. Clobbers x2, x3.
  void emit_page_lookup(u32 waddr, bool store, std::vector<size_t>& fail) {
    assert(!in_cold());
    emit_walk_probe(waddr);
    e().lsr_imm(SCRATCH2, waddr, mem::PAGE_SHIFT);
    e().ldr_x_reg(SCRATCH2, R_PT, SCRATCH2, true, true);
    if (store) {
      e().lsr_imm(SCRATCH3, SCRATCH2, 62, true);
      fail.push_back(e().cbnz_fwd(SCRATCH3));
    }
    e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
    fail.push_back(e().cbz_fwd(SCRATCH3, true));
  }
  // Slow-path epilogue: the helper returned the aligned raw value in w0
  // (word, halfword or byte); reproduce the rotation / extension into `dst`.
  void emit_load_post(Mem m, u32 dst) {
    switch (m) {
    case Mem::Ld32:
      e().lsl_imm(SCRATCH5, SCRATCH1, 3);
      e().rorv(dst, SCRATCH0, SCRATCH5);
      break;
    case Mem::Ld16:
      if (a9_) { if (dst != SCRATCH0) e().mov(dst, SCRATCH0); }
      else { e().ubfiz(SCRATCH5, SCRATCH1, 3, 1); e().rorv(dst, SCRATCH0, SCRATCH5); }
      break;
    case Mem::Ld8:  if (dst != SCRATCH0) e().mov(dst, SCRATCH0); break;
    case Mem::Ld8S: e().sxtb(dst, SCRATCH0); break;
    case Mem::Ld16S:
      if (a9_) e().sxth(dst, SCRATCH0);
      else {
        size_t odd = e().tbnz_fwd(SCRATCH1, 0);
        e().sxth(dst, SCRATCH0);
        size_t j = e().b_fwd();
        e().bind(odd);
        e().lsr_imm(SCRATCH0, SCRATCH0, 8);
        e().sxtb(dst, SCRATCH0);
        e().bind(j);
      }
      break;
    default: break;
    }
  }
  // DS_JIT_COSTPROBE selects both CPUs (1) or one of them (9 / 7).
  bool costprobe_on() const {
    const int c = rt().costprobe;
    return c == 1 || (c == 9 && a9_) || (c == 7 && !a9_);
  }
  // Data cost of one access at `waddr` into `wcost`.
  void emit_data_cost(u32 waddr, u32 wcost, bool word, bool seq, bool store) {
    // ARM9 entries are 8 bytes: loads at [1..3], stores at [5..7] (see CpuContext::timing9).
    const u32 k = a9_ ? ((store ? 4 : 0) + (seq ? 3 : (word ? 2 : 1))) : (seq ? (word ? 3 : 1) : (word ? 2 : 0));
    // DS_JIT_COSTPROBE: emit the lookup twice; the second overwrites the first,
    // so the value used is unchanged and only the cost of computing it doubles.
    for (int rep = (costprobe_on() && (rt().costprobe_part & 1)) ? 1 : 0; rep >= 0; --rep) {
      e().lsr_imm(wcost, waddr, a9_ ? 12 : 15);
      e().add_reg(wcost, R_TIM, wcost, LSL, a9_ ? 3 : 2, true);
      e().ldrb(wcost, wcost, k);
    }
  }

  // Slot in mem::Timing's precomputed ARM7 cost table for a single access with
  // these translate-time constants, or -1 when the table does not cover this
  // block's code-fetch cost and the caller must keep the inline model.
  int cost7_slot(bool cdi, bool word) const {
    if (a9_ || rt().nocost7) return -1;
    const int ni = cpu_.nds->bus.timing().nc7_index(numC_nonseq7());
    if (ni < 0) return -1;
    return static_cast<int>(mem::Timing::cost7_offset(code_region7_ == 0x02, cdi, static_cast<u32>(ni), word));
  }
  // wd = max(wa, wb) without flags: wa + max(wb - wa, 0)
  void emit_max(u32 wd, u32 wa, u32 wb, u32 tmp) {
    e().sub_reg(tmp, wb, wa);
    e().bic_reg(tmp, tmp, tmp, ASR, 31);
    e().add_reg(wd, wa, tmp);
  }
  // Charge a CD / CDI instruction: data cost in `wd`, address in `waddr`
  // (ARM7 main-RAM rule). `wd` is w6; temporaries w4 (ARM9) or w2-w5 (ARM7).
  void emit_charge_data(u32 wd, u32 waddr, bool cdi) {
    flush_pending();
    // DS_JIT_COSTPROBE: a second copy charged to a dead scratch instead of the
    // budget. The emitted work doubles, the emulated cycle count does not.
    if (costprobe_on() && (rt().costprobe_part & 2)) emit_charge_data_body(wd, waddr, cdi, SCRATCH3);
    emit_charge_data_body(wd, waddr, cdi, R_BUDGET);
  }
  void emit_charge_data_body(u32 wd, u32 waddr, bool cdi, u32 wbudget) {
    if (rt().fastcost) {   // DS_JIT_FASTCOST: measurement knob, inexact: numC + numD
      e().add_imm(SCRATCH5, wd, a9_ ? numC(pc_) : numC_nonseq7());
      e().sub_reg(wbudget, wbudget, SCRATCH5);
      return;
    }
    if (a9_) {
      // cost = max(nc + nd - 6, nc, nd) with nd >= 1 (timing tables never
      // hold 0): nc <= 1 -> nd; nc <= 6 -> max(nc, nd); else nc + max(nd - 6, 0).
      const u32 nc = numC(pc_);
      if (nc <= 1) { e().sub_reg(wbudget, wbudget, wd); return; }
      if (nc <= 6) {
        e().sub_imm(SCRATCH4, wd, nc);
        e().bic_reg(SCRATCH4, SCRATCH4, SCRATCH4, ASR, 31);
        e().add_imm(SCRATCH4, SCRATCH4, nc);
      } else {
        e().sub_imm(SCRATCH4, wd, 6);
        e().bic_reg(SCRATCH4, SCRATCH4, SCRATCH4, ASR, 31);
        e().add_imm(SCRATCH4, SCRATCH4, nc);
      }
      e().sub_reg(wbudget, wbudget, SCRATCH4);
      return;
    }
    // ARM7 (temps w2-w5; `wd` is w6 and w7 holds a writeback value).
    const u32 nc = numC_nonseq7();
    const bool code_main = code_region7_ == 0x02;
    e().lsr_imm(SCRATCH2, waddr, 24);
    e().eor_imm(SCRATCH2, SCRATCH2, 2);
    size_t not_main = e().cbnz_fwd(SCRATCH2);
    if (code_main) {
      e().add_imm(SCRATCH5, wd, nc);
    } else {
      const u32 ncx = cdi ? nc + 1 : nc;
      e().mov_imm(SCRATCH3, ncx);
      emit_max(SCRATCH5, SCRATCH3, wd, SCRATCH4);
      if (ncx >= 3) e().add_imm(SCRATCH3, wd, ncx - 3); else e().sub_imm(SCRATCH3, wd, 3 - ncx);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH3, SCRATCH4);
    }
    size_t done = e().b_fwd();
    e().bind(not_main);
    if (code_main) {
      if (cdi) e().add_imm(SCRATCH2, wd, 1); else e().mov(SCRATCH2, wd);
      e().mov_imm(SCRATCH3, nc);
      emit_max(SCRATCH5, SCRATCH3, SCRATCH2, SCRATCH4);
      if (nc >= 3) e().add_imm(SCRATCH3, SCRATCH2, nc - 3); else e().sub_imm(SCRATCH3, SCRATCH2, 3 - nc);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH3, SCRATCH4);
    } else {
      e().add_imm(SCRATCH5, wd, nc + (cdi ? 1 : 0));
    }
    e().bind(done);
    e().sub_reg(wbudget, wbudget, SCRATCH5);
  }
  // One load or store at the address in w1. The fast path inlines the
  // page-table lookup and the access; the cold path calls the slow helper
  // (MMIO, unmapped, read-only and code pages), then both run the tail
  // (writeback for stores, cost), and the cold path polls before rejoining.
  // The hot path issues the timing-table load and the address arithmetic
  // between the page-entry load and its first use, and the charge between
  // the data load and its use, so the in-order core stalls on neither.
  // `const_nd` >= 0: the data cost is a translate-time constant (a
  // pc-relative literal's page, or --cpu-oc), so the whole charge joins the
  // block's static cycles and the access emits no cost lookup at all. The
  // charge lands before the access instead of after it; nothing observes the
  // budget between the two (the strict-mode check is per instruction).
  void emit_single(Mem m, u32 wdata, u32 dst, bool wb, u32 wb_reg, bool cdi, int const_nd = -1) {
    const bool word = is_word(m);
    if (rt().cpu_oc && const_nd < 0) const_nd = static_cast<int>(oc_data_cost(word, false, !is_load(m)));
    const bool const_cost = const_nd >= 0;
    if (const_cost) add_pending(const_charge(static_cast<u32>(const_nd), cdi));
    flush_pending();
    const int slot7 = const_cost ? -1 : cost7_slot(cdi, word);
    auto cost = [&] {
      if (const_cost) return;
      if (slot7 < 0) { emit_data_cost(SCRATCH1, SCRATCH6, word, false, !is_load(m)); return; }
      e().lsr_imm(SCRATCH6, SCRATCH1, 15);
      e().add_reg(SCRATCH6, R_TIM, SCRATCH6, LSL, 5, true);
      e().add_imm(SCRATCH6, SCRATCH6, mem::Timing::COST7_OFFSET, true);
      e().ldrb(SCRATCH6, SCRATCH6, static_cast<u32>(slot7));
    };
    auto charge = [&] {
      if (wb) e().mov(host_reg(wb_reg), SCRATCH7);
      if (const_cost) return;
      if (slot7 < 0) { emit_charge_data(SCRATCH6, SCRATCH1, cdi); return; }
      flush_pending();
      e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH6);
    };
    std::vector<size_t> fail;
    emit_walk_probe(SCRATCH1);
    e().lsr_imm(SCRATCH2, SCRATCH1, mem::PAGE_SHIFT);
    e().ldr_x_reg(SCRATCH2, R_PT, SCRATCH2, true, true);
    cost();
    if (word) e().and_imm(SCRATCH4, SCRATCH1, ~3u);
    else if (m == Mem::Ld16 || m == Mem::St16 || (m == Mem::Ld16S && a9_)) e().and_imm(SCRATCH4, SCRATCH1, ~1u);
    if (m == Mem::Ld32 && a9_) e().lsl_imm(SCRATCH5, SCRATCH1, 3);
    if (!is_load(m)) {
      e().lsr_imm(SCRATCH3, SCRATCH2, 62, true);
      fail.push_back(e().cbnz_fwd(SCRATCH3));
    }
    e().lsl_imm(SCRATCH3, SCRATCH2, 2, true);
    fail.push_back(e().cbz_fwd(SCRATCH3, true));
    switch (m) {
    case Mem::Ld32:
      e().ldr_w_reg(SCRATCH0, SCRATCH3, SCRATCH4);
      charge();
      if (!a9_) e().lsl_imm(SCRATCH5, SCRATCH1, 3);
      e().rorv(dst, SCRATCH0, SCRATCH5);
      break;
    case Mem::Ld16:
      if (a9_) { e().ldrh_reg(dst, SCRATCH3, SCRATCH4); charge(); }
      else { e().ldrh_reg(SCRATCH0, SCRATCH3, SCRATCH4); charge(); e().ubfiz(SCRATCH5, SCRATCH1, 3, 1); e().rorv(dst, SCRATCH0, SCRATCH5); }
      break;
    case Mem::Ld8:  e().ldrb_reg(dst, SCRATCH3, SCRATCH1); charge(); break;
    case Mem::Ld8S: e().ldrsb_w_reg(dst, SCRATCH3, SCRATCH1); charge(); break;
    case Mem::Ld16S:
      if (a9_) { e().ldrsh_w_reg(dst, SCRATCH3, SCRATCH4); charge(); }
      else {
        size_t odd = e().tbnz_fwd(SCRATCH1, 0);
        e().ldrsh_w_reg(dst, SCRATCH3, SCRATCH1);
        size_t j = e().b_fwd();
        e().bind(odd);
        e().ldrsb_w_reg(dst, SCRATCH3, SCRATCH1);
        e().bind(j);
        charge();
      }
      break;
    case Mem::St32: e().str_w_reg(wdata, SCRATCH3, SCRATCH4); charge(); break;
    case Mem::St16: e().strh_reg(wdata, SCRATCH3, SCRATCH4); charge(); break;
    case Mem::St8:  e().strb_reg(wdata, SCRATCH3, SCRATCH1); charge(); break;
    }
    const size_t join = hot_.size();
    cold_begin(fail);
    if (!is_load(m) && wdata != SCRATCH2) e().mov(SCRATCH2, wdata);
    call_stub(is_load(m) ? rt().slow_load[size_index(m)] : rt().slow_store[size_index(m)]);
    if (is_load(m)) emit_load_post(m, dst);
    cost();
    charge();
    call_stub(rt().poll);
    e().word(make_key(pc_ + step(), thumb_));
    cold_end_jump(join);
  }

  // Multi-register transfer, fast path in one 2 KB page; the cold path is
  // the interpreter. `list` is 16 bits; `wstart` (w1) = lowest address,
  // `wwb` (w7) = writeback value. `rn` = base register for the STM "Rn in
  // list" rule (or 16 for none).
  void emit_block_transfer(u32 instr, u32 list, bool load, bool writeback, u32 rn, bool interwork_pc, u32 pc_store_value) {
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    flush_pending();
    std::vector<size_t> fail;
    e().add_imm(SCRATCH2, SCRATCH1, n * 4 - 1);
    e().eor_reg(SCRATCH2, SCRATCH2, SCRATCH1);
    e().lsr_imm(SCRATCH2, SCRATCH2, mem::PAGE_SHIFT);
    fail.push_back(e().cbnz_fwd(SCRATCH2));
    emit_page_lookup(SCRATCH1, !load, fail);
    e().and_imm(SCRATCH2, SCRATCH1, ~3u);          // each word access is aligned; the base may not be (Thumb)
    e().add_uxtw(SCRATCH3, SCRATCH3, SCRATCH2);
    u32 k = 0;
    bool first = true;
    const bool pc_in_list = (list & 0x8000) != 0;
    if (load) {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        e().ldr_w(i == 15 ? SCRATCH0 : host_reg(i), SCRATCH3, 4 * k);
        ++k;
      }
      if (writeback && !(list & (1u << rn))) e().mov(host_reg(rn), SCRATCH7);
    } else {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        u32 src;
        if (i == 15) { e().mov_imm(SCRATCH0, pc_store_value); src = SCRATCH0; }
        else if (i == rn && !first && writeback) src = SCRATCH7;
        else src = host_reg(i);
        e().str_w(src, SCRATCH3, 4 * k);
        ++k; first = false;
      }
      if (writeback) e().mov(host_reg(rn), SCRATCH7);
    }
    // cost: N + (n - 1) S from the page's entry (--cpu-oc: main RAM's, at translate time)
    const bool oc = rt().cpu_oc;
    u32 oc_nd = 0;
    if (oc) oc_nd = oc_data_cost(true, false, !load) + (n - 1) * oc_data_cost(true, true, !load);
    else {
      emit_data_cost(SCRATCH1, SCRATCH6, true, false, !load);
      if (n > 1) {
        emit_data_cost(SCRATCH1, SCRATCH5, true, true, !load);
        e().mov_imm(SCRATCH4, n - 1);
        e().madd(SCRATCH6, SCRATCH5, SCRATCH4, SCRATCH6);
      }
    }
    if (load && pc_in_list) {
      // The interpreter charges the CDI cost after the jump: the stub does it
      // from the new pc/state (w1 = numD, w2 = data address for the ARM7 rule).
      e().mov(SCRATCH2, SCRATCH1);
      if (oc) e().mov_imm(SCRATCH1, oc_nd); else e().mov(SCRATCH1, SCRATCH6);
      emit_branch_indirect(SCRATCH0, interwork_pc, true);
      cold_begin(fail);
      emit_fallback(instr, true);
      cold_end();
      ended_ = true;
      return;
    }
    if (oc) { add_pending(const_charge(oc_nd, load)); flush_pending(); }
    else emit_charge_data(SCRATCH6, SCRATCH1, load);
    const size_t join = hot_.size();
    cold_begin(fail);
    emit_fallback(instr, false);
    cold_end_jump(join);
  }

  // Effective address of a single transfer -> w1; the writeback value -> w7
  // when there is one (post-indexed: w1 = base, w7 = base +/- offset).
  void emit_ea(u32 rn, const Operand& off, bool pre, bool up, bool writeback) {
    const u32 hb = to_reg(reg_operand(rn, pc_ + 8), SCRATCH6);
    const u32 dst = pre && !writeback ? SCRATCH1 : SCRATCH7;
    if (off.imm) { if (up) e().add_imm_any(dst, hb, off.value, SCRATCH5); else e().sub_imm_any(dst, hb, off.value, SCRATCH5); }
    else { if (up) e().add_reg(dst, hb, off.reg); else e().sub_reg(dst, hb, off.reg); }
    if (dst == SCRATCH7) { if (pre) e().mov(SCRATCH1, SCRATCH7); else e().mov(SCRATCH1, hb); }
  }

  // ---- drivers -------------------------------------------------------------------------------------------
  void translate_arm(u32 instr);
  void translate_thumb(u16 instr);
  void arm_data_processing(u32 instr, AOp op);
  void arm_multiply(u32 instr, AOp op);
  void arm_dsp_multiply(u32 instr, AOp op);
  void arm_ldr_str(u32 instr, AOp op);
  void arm_ldr_str_h(u32 instr, AOp op);
  void arm_ldm_stm(u32 instr, bool load);
  void arm_msr(u32 instr, AOp op);
  void emit_mul_cycles7(u32 wrs, bool signed_op, u32 extra);
  // Multiplies that set flags leave C alone on the ARM9 and clear it on the ARM7 (melonDS).
  Carry mul_carry() const { return a9_ ? Carry{} : Carry{CarryKind::Const, 0, 0}; }
  void thumb_alu(u16 instr);
  void thumb_ldr_str(u16 instr, TOp op);
};

// ---- ARM -----------------------------------------------------------------------------------------------------

void Translator::arm_data_processing(u32 instr, AOp op) {
  const u32 opcode = (instr >> 21) & 0xF;
  const bool s = instr & (1u << 20);
  const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
  const bool test = opcode >= 8 && opcode <= 0xB;
  const bool logical = opcode <= 1 || opcode == 8 || opcode == 9 || opcode >= 0xC;
  if (rd == 15 && !test) { emit_fallback(instr, true); return; }
  if (op == AOp::DpRegShift) add_pending(numC_internal() + 1);
  else add_pending(numC(pc_));

  const bool want_carry = s && logical && (live_ & F_C);
  Carry carry;
  Operand op2, rn_v;
  if (op == AOp::DpImm) {
    const u32 imm = instr & 0xFF, rot = (instr >> 8) & 0xF;
    const u32 v = rotr(imm, rot * 2);
    op2 = {true, 0, v};
    carry = rot ? Carry{CarryKind::Const, v >> 31, 0} : Carry{};
    rn_v = reg_operand(rn, pc_ + 8);
  } else if (op == AOp::DpImmShift) {
    op2 = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, reg_operand(instr & 0xF, pc_ + 8), want_carry, carry, SCRATCH2, SCRATCH3);
    rn_v = reg_operand(rn, pc_ + 8);
  } else {
    op2 = shift_reg((instr >> 5) & 3, host_reg((instr >> 8) & 0xF), reg_operand(instr & 0xF, pc_ + 12), want_carry, carry, SCRATCH2, SCRATCH3);
    rn_v = reg_operand(rn, pc_ + 12);
  }

  // csel form: compute into a scratch so the guest register is only written by
  // the select that follows (dp_csel_, set by translate_arm).
  const u32 dst = (test || dp_csel_) ? SCRATCH6 : host_reg(rd);
  const u32 a = to_reg(rn_v, SCRATCH4);
  auto b_reg = [&]() { return to_reg(op2, SCRATCH5); };

  switch (opcode) {
  case 0x0: case 0x8:
    if (op2.imm) { if (!e().and_imm(dst, a, op2.value)) e().and_reg(dst, a, b_reg()); }
    else e().and_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x1: case 0x9:
    if (op2.imm) { if (!e().eor_imm(dst, a, op2.value)) e().eor_reg(dst, a, b_reg()); }
    else e().eor_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xC:
    if (op2.imm) { if (!e().orr_imm(dst, a, op2.value)) e().orr_reg(dst, a, b_reg()); }
    else e().orr_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xE:
    if (op2.imm) { if (!e().and_imm(dst, a, ~op2.value)) e().bic_reg(dst, a, b_reg()); }
    else e().bic_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xD:
    if (op2.imm) e().mov_imm(dst, op2.value); else if (op2.reg != dst) e().mov(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xF:
    if (op2.imm) e().mov_imm(dst, ~op2.value); else e().mvn(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x2: case 0xA: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e().sub_imm_any(d, a, op2.value, SCRATCH5, s); else e().add_sub(true, s, d, a, op2.reg);
    break;
  }
  case 0x4: case 0xB: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e().add_imm_any(d, a, op2.value, SCRATCH5, s); else e().add_sub(false, s, d, a, op2.reg);
    break;
  }
  case 0x3:
    if (op2.imm && op2.value == 0) { if (s) e().negs(dst, a); else e().neg(dst, a); }
    else e().add_sub(true, s, dst, b_reg(), a);
    break;
  case 0x5: e().adc(dst, a, b_reg(), s); break;
  case 0x6: e().sbc(dst, a, b_reg(), s); break;
  case 0x7: e().sbc(dst, b_reg(), a, s); break;
  }
}

void Translator::emit_mul_cycles7(u32 wrs, bool signed_op, u32 extra) {
  // 1..4 internal cycles from the magnitude of rs (signed: of rs ^ (rs >> 31)).
  if (signed_op) e().eor_reg(SCRATCH6, wrs, wrs, ASR, 31); else e().mov(SCRATCH6, wrs);
  e().clz(SCRATCH6, SCRATCH6);
  e().lsr_imm(SCRATCH6, SCRATCH6, 3);          // leading zero bytes 0..4
  e().lsl_imm(SCRATCH6, SCRATCH6, 2);
  e().mov_imm(SCRATCH7, 0x11234);              // nibble table: 4,3,2,1,1
  e().lsrv(SCRATCH7, SCRATCH7, SCRATCH6);
  e().and_imm(SCRATCH7, SCRATCH7, 0xF);
  if (extra) e().add_imm(SCRATCH7, SCRATCH7, extra);
  e().sub_reg(R_BUDGET, R_BUDGET, SCRATCH7);
}

void Translator::arm_multiply(u32 instr, AOp op) {
  const bool s = instr & (1u << 20);
  const u32 rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  const u32 hrs = host_reg(rs), hrm = host_reg(rm);
  if (op == AOp::Mul || op == AOp::Mla) {
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF;
    if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, true, op == AOp::Mla ? 1 : 0); }
    if (op == AOp::Mla) e().madd(host_reg(rd), hrm, hrs, host_reg(rn)); else e().mul(host_reg(rd), hrm, hrs);
    if (s) set_flags_logical(host_reg(rd), mul_carry());
    return;
  }
  const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF;
  const bool sgn = op == AOp::Smull || op == AOp::Smlal;
  const bool acc = op == AOp::Umlal || op == AOp::Smlal;
  if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
  else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, sgn, 1); }
  if (acc) {
    e().mov(SCRATCH0, host_reg(rdlo));
    e().bfi(SCRATCH0, host_reg(rdhi), 32, 32, true);
    if (sgn) e().smaddl(SCRATCH0, hrm, hrs, SCRATCH0); else e().umaddl(SCRATCH0, hrm, hrs, SCRATCH0);
  } else {
    if (sgn) e().smull(SCRATCH0, hrm, hrs); else e().umull(SCRATCH0, hrm, hrs);
  }
  e().mov(host_reg(rdlo), SCRATCH0);
  e().lsr_imm(host_reg(rdhi), SCRATCH0, 32, true);
  if (s) set_flags_logical(SCRATCH0, mul_carry(), true);
}

// v5TE DSP multiplies (SMULxy / SMLAxy / SMULWy / SMLAWy), ARM9 only.
//
// The accumulating forms set the sticky Q flag on signed overflow of the
// accumulate and leave NZCV alone. The guest NZCV live in the host NZCV, so
// the overflow cannot be taken from an `adds` -- it is computed the usual
// arithmetic way, ((a^sum) & (b^sum)) >> 31, and OR'd into the CPSR image in
// memory, which is exactly what the interpreter does.
//
// Cost is charge_C -- numC and no internal cycles -- which is why these are in
// arm_simple_cost and a conditional form precharges before the skip branch.
void Translator::arm_dsp_multiply(u32 instr, AOp op) {
  const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF,
            rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  const bool acc = op == AOp::SmlaXY || op == AOp::SmlawY;
  const bool wide = op == AOp::SmlawY || op == AOp::SmulwY;
  add_pending(numC(pc_));
  e().sbfx(SCRATCH1, host_reg(rs), (instr & (1u << 6)) ? 16 : 0, 16);
  if (wide) {
    // ((s64)(s32)Rm * half(Rs, y)) >> 16, then truncated to 32 bits.
    e().smull(SCRATCH0, host_reg(rm), SCRATCH1);
    e().asr_imm(SCRATCH0, SCRATCH0, 16, true);
  } else {
    e().sbfx(SCRATCH0, host_reg(rm), (instr & (1u << 5)) ? 16 : 0, 16);
    e().mul(SCRATCH0, SCRATCH0, SCRATCH1);
  }
  if (!acc) { e().mov(host_reg(rd), SCRATCH0); return; }
  e().add_reg(SCRATCH2, SCRATCH0, host_reg(rn));
  e().eor_reg(SCRATCH3, SCRATCH0, SCRATCH2);
  e().eor_reg(SCRATCH4, host_reg(rn), SCRATCH2);
  e().and_reg(SCRATCH3, SCRATCH3, SCRATCH4);
  e().lsr_imm(SCRATCH3, SCRATCH3, 31);
  e().ldr_w(SCRATCH4, R_CTX, OFF_CPSR);
  e().orr_reg(SCRATCH4, SCRATCH4, SCRATCH3, LSL, 27);
  e().str_w(SCRATCH4, R_CTX, OFF_CPSR);
  e().mov(host_reg(rd), SCRATCH2);   // last: rd may alias rm/rs/rn
}

void Translator::arm_ldr_str(u32 instr, AOp op) {
  const bool l = instr & (1u << 20), b = instr & (1u << 22);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const bool writeback = !p || w;
  Operand off;
  if (op == AOp::LdrStrImm) off = {true, 0, instr & 0xFFF};
  else { Carry c; off = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, reg_operand(instr & 0xF, pc_ + 8), false, c, SCRATCH2, SCRATCH3); }
  emit_ea(rn, off, p, u, writeback);

  if (l) {
    if (writeback) e().mov(host_reg(rn), SCRATCH7);   // before the load: with rd == rn the loaded value wins
    // ldr rd, [pc, #imm] (ARM9): the literal's address, hence its page's N32
    // cost, is known now. Baked; the block depends on that page's data byte.
    int const_nd = -1;
    if (a9_ && !b && op == AOp::LdrStrImm && rn == 15 && !writeback) {
      const u32 addr = u ? pc_ + 8 + (instr & 0xFFF) : pc_ + 8 - (instr & 0xFFF);
      note_dep(addr, mem::Timing::RETIME_DATA);
      const_nd = cpu_.timing9[addr >> 12][2];
    }
    emit_single(b ? Mem::Ld8 : Mem::Ld32, 0, host_reg(rd), false, 0, true, const_nd);
  } else {
    u32 data = host_reg(rd);
    if (rd == 15) { e().mov_imm(SCRATCH0, pc_ + 12); data = SCRATCH0; }
    emit_single(b ? Mem::St8 : Mem::St32, data, 0, writeback, rn, false);
  }
}

void Translator::arm_ldr_str_h(u32 instr, AOp op) {
  const bool l = instr & (1u << 20);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 sh = (instr >> 5) & 3;
  const bool writeback = !p || w;
  Operand off;
  if (op == AOp::LdrStrHImm) off = {true, 0, ((instr >> 4) & 0xF0) | (instr & 0xF)};
  else off = {false, host_reg(instr & 0xF), 0};
  emit_ea(rn, off, p, u, writeback);

  if (l) {
    const Mem m = sh == 1 ? Mem::Ld16 : sh == 2 ? Mem::Ld8S : Mem::Ld16S;
    if (writeback) e().mov(host_reg(rn), SCRATCH7);
    emit_single(m, 0, host_reg(rd), false, 0, true);
  } else {
    emit_single(Mem::St16, host_reg(rd), 0, writeback, rn, false);
  }
}

void Translator::arm_ldm_stm(u32 instr, bool load) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  const u32 list = instr & 0xFFFF;
  const u32 n = static_cast<u32>(__builtin_popcount(list));
  const u32 hb = host_reg(rn);
  // start address -> w1, writeback value -> w7
  if (u) {
    if (p) e().add_imm(SCRATCH1, hb, 4); else e().mov(SCRATCH1, hb);
    e().add_imm(SCRATCH7, hb, n * 4);
  } else {
    if (p) e().sub_imm(SCRATCH1, hb, n * 4); else e().sub_imm(SCRATCH1, hb, n * 4 - 4);
    e().sub_imm(SCRATCH7, hb, n * 4);
  }
  e().and_imm(SCRATCH1, SCRATCH1, ~3u);
  emit_block_transfer(instr, list, load, w, rn, a9_, pc_ + 12);
}

// MSR CPSR_<fields>, Rm / #imm. Inline when the mode stays the same and the
// CPU is not in user mode (both tested at run time); the control field then
// only moves I and F, the other fields merge into the memory copy, and the
// flags field sets the host NZCV. Everything else runs through the
// interpreter on the cold path. T is never written (as in the interpreter).
void Translator::arm_msr(u32 instr, AOp op) {
  const u32 fields = (instr >> 16) & 0xF;
  flush_pending();                 // the cold path charges through the interpreter; numC is charged after the tests
  u32 wv;
  if (op == AOp::MsrImm) { e().mov_imm(SCRATCH4, rotr(instr & 0xFF, ((instr >> 8) & 0xF) * 2)); wv = SCRATCH4; }
  else wv = host_reg(instr & 0xF);
  std::vector<size_t> fail;
  const u32 mem_mask = ((fields & 1) ? 0xC0u : 0) | ((fields & 2) ? 0xFF00u : 0) | ((fields & 4) ? 0xFF0000u : 0) | ((fields & 8) ? 0x0F000000u : 0);
  const bool touch_mem = mem_mask != 0 || (fields & 7);
  if (touch_mem) e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
  if (fields & 7) {
    e().and_imm(SCRATCH3, SCRATCH2, 0xF);
    fail.push_back(e().cbz_fwd(SCRATCH3));              // user mode: flags only (interpreter)
    if (fields & 1) {
      e().eor_reg(SCRATCH3, SCRATCH2, wv);
      e().and_imm(SCRATCH3, SCRATCH3, 0x1F);
      fail.push_back(e().cbnz_fwd(SCRATCH3));           // mode change: bank switch (interpreter)
    }
  }
  add_pending(numC(pc_));
  flush_pending();
  if (mem_mask) {
    if (mem_mask == 0xC0) { e().ubfx(SCRATCH3, wv, 6, 2); e().bfi(SCRATCH2, SCRATCH3, 6, 2); }
    else {
      e().mov_imm(SCRATCH5, mem_mask);
      e().and_reg(SCRATCH3, wv, SCRATCH5);
      e().bic_reg(SCRATCH2, SCRATCH2, SCRATCH5);
      e().orr_reg(SCRATCH2, SCRATCH2, SCRATCH3);
    }
    e().str_w(SCRATCH2, R_CTX, OFF_CPSR);
  }
  if (fields & 8) e().msr_nzcv(wv);
  if (fields & 1) { call_stub(rt().poll); e().word(make_key(pc_ + 4, thumb_)); }   // I may have changed
  if (fail.empty()) return;
  const size_t join = hot_.size();
  cold_begin(fail);
  emit_fallback(instr, false);
  cold_end_jump(join);
}

// Conditional instructions that can be predicated with a select rather than a
// branch: data processing with a register destination and no flag write, whose
// body is a pure computation into that register. Register-shifted operands are
// excluded -- their cycle cost is not the plain numC the select form charges.
bool use_csel_op(AOp op, u32 instr) {
  if (op != AOp::DpImm && op != AOp::DpImmShift) return false;
  if (instr & (1u << 20)) return false;                 // S: writes flags
  const u32 opcode = (instr >> 21) & 0xF;
  if (opcode >= 8 && opcode <= 0xB) return false;        // TST/TEQ/CMP/CMN: no destination
  return ((instr >> 12) & 0xF) != 15;                    // PC destination falls back
}

// Instructions whose whole cost is the static fetch cost (plus static
// extras): a conditional one charges numC before the condition test, so the
// skipped path needs no code of its own.
bool arm_simple_cost(AOp op) {
  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: case AOp::Mrs: case AOp::Clz: case AOp::Pld: case AOp::Mcr:
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    return true;
  default: return false;
  }
}

void Translator::translate_arm(u32 instr) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) {
    if (((instr >> 25) & 7) == 5 && a9_) {       // BLX imm
      s32 off = static_cast<s32>(instr << 8) >> 6;
      off |= (instr >> 23) & 2;
      e().mov_imm(host_reg(14), pc_ + 4);
      emit_branch_static((pc_ + 8 + static_cast<u32>(off)) & ~1u, true, true);
      return;
    }
    if (((instr >> 24) & 0xF7) == 0x55) { add_pending(numC(pc_)); return; }   // PLD
    emit_fallback(instr, true);
    return;
  }
  const AOp op = arm::decode_arm(instr);

  if (arm_needs_fallback(instr, a9_)) {
    // The helper evaluates the condition itself. Jumps are possible for
    // PC-destination forms, exceptions and coprocessor/MSR side effects.
    bool always = false;
    switch (op) {
    case AOp::Swi: case AOp::Bkpt: case AOp::Undefined: case AOp::Cdp: case AOp::Ldc: case AOp::Stc:
      always = cond == 0xE; break;
    case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg: case AOp::Ldm:
      always = cond == 0xE && (op == AOp::Ldm ? (instr & 0x8000) != 0 : (((instr >> 12) & 0xF) == 15 && (op != AOp::LdrStrImm && op != AOp::LdrStrReg && op != AOp::LdrStrHImm && op != AOp::LdrStrHReg ? true : (instr & (1u << 20)) != 0)));
      break;
    default: break;
    }
    emit_fallback(instr, always);
    return;
  }

  size_t skip = 0;
  const bool conditional = cond != 0xE;
  // Predicate with a `csel` instead of branching around the body, for the
  // shapes where the body is a plain value computation: data processing that
  // writes a register and not the flags. The result goes to a scratch and the
  // select commits it, so the guest register is untouched when the condition
  // fails. One never-mispredicting instruction replaces a data-dependent
  // branch, and the cost is charged unconditionally -- which is what the
  // instruction costs either way, and what `precharged` already arranged.
  const bool csel_form = conditional && !rt().nocsel && use_csel_op(op, instr);
  const bool precharged = conditional && !csel_form && arm_simple_cost(op);
  if (conditional && !csel_form) {
    if (precharged) { const u32 c = numC(pc_); add_pending(c); flush_pending(); charged_ahead_ = c; }
    else flush_pending();
    skip = hot_.b_cond_fwd(invert(static_cast<Cond>(cond)));
  }
  dp_csel_ = csel_form;

  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    arm_data_processing(instr, op);
    break;
  case AOp::Mrs: {
    const u32 rd = (instr >> 12) & 0xF;
    add_pending(numC(pc_));
    if (rd == 15) break;
    if (instr & (1u << 22)) e().ldr_w(host_reg(rd), R_CTX, OFF_SPSR);
    else {
      e().mrs_nzcv(SCRATCH1);
      e().ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
      e().and_imm(SCRATCH2, SCRATCH2, 0x0FFFFFFF);
      e().orr_reg(host_reg(rd), SCRATCH2, SCRATCH1);
    }
    break;
  }
  case AOp::B: case AOp::Bl: {
    const s32 off = static_cast<s32>(instr << 8) >> 6;
    if (op == AOp::Bl) e().mov_imm(host_reg(14), pc_ + 4);
    emit_branch_static(pc_ + 8 + static_cast<u32>(off), false, true);
    break;
  }
  case AOp::Bx: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e().mov_imm(SCRATCH0, pc_ + 8); else e().mov(SCRATCH0, host_reg(rm));
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::BlxReg: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e().mov_imm(SCRATCH0, pc_ + 8); else e().mov(SCRATCH0, host_reg(rm));
    e().mov_imm(host_reg(14), pc_ + 4);
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::Clz: {
    const u32 rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    add_pending(numC(pc_));
    if (rd == 15 || rm == 15) break;
    e().clz(host_reg(rd), host_reg(rm));
    break;
  }
  case AOp::Pld: add_pending(numC(pc_)); break;
  case AOp::Mcr: add_pending(numC(pc_) + 2); break;      // ignored cache operation: charge_CI(2)
  case AOp::MsrReg: case AOp::MsrImm: arm_msr(instr, op); break;
  case AOp::SmlaXY: case AOp::SmulXY: case AOp::SmlawY: case AOp::SmulwY:
    arm_dsp_multiply(instr, op);
    break;
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    arm_multiply(instr, op);
    break;
  case AOp::LdrStrImm: case AOp::LdrStrReg: arm_ldr_str(instr, op); break;
  case AOp::LdrStrHImm: case AOp::LdrStrHReg: arm_ldr_str_h(instr, op); break;
  case AOp::Ldm: arm_ldm_stm(instr, true); break;
  case AOp::Stm: arm_ldm_stm(instr, false); break;
  default: break;
  }

  if (csel_form) {
    dp_csel_ = false;
    const u32 rd = (instr >> 12) & 0xF;
    e().csel(host_reg(rd), SCRATCH6, host_reg(rd), static_cast<Cond>(cond));
    return;
  }

  if (conditional) {
    if (ended_) {
      hot_.bind(skip);
      add_pending(numC(pc_));
      emit_branch_static(pc_ + 4, false, false);
    } else if (precharged) {
      flush_pending();
      assert(charged_ahead_ == 0 && "a simple-cost instruction charged less than numC");
      charged_ahead_ = 0;
      hot_.bind(skip);
    } else {
      flush_pending();
      size_t join = hot_.b_fwd();
      hot_.bind(skip);
      add_pending(numC(pc_));
      flush_pending();
      hot_.bind(join);
    }
  }
}

// ---- Thumb -------------------------------------------------------------------------------------------------------

void Translator::thumb_alu(u16 instr) {
  const u32 rs = (instr >> 3) & 7, rd = instr & 7;
  const u32 hs = host_reg(rs), hd = host_reg(rd);
  const u32 aluop = (instr >> 6) & 0xF;
  switch (aluop) {
  case 0x2: case 0x3: case 0x4: case 0x7: {   // LSL LSR ASR ROR by register
    add_pending(numC_internal() + 1);
    Carry c;
    const u32 type = aluop == 2 ? 0 : aluop == 3 ? 1 : aluop == 4 ? 2 : 3;
    Operand r = shift_reg(type, hs, Operand{false, hd, 0}, live_ & F_C, c, SCRATCH2, SCRATCH3);
    e().mov(hd, r.reg);
    set_flags_logical(hd, c);
    return;
  }
  case 0xD: {   // MUL
    if (a9_) add_pending(numC(pc_) + 3);
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hd, true, 0); }
    e().mul(hd, hd, hs);
    set_flags_logical(hd, mul_carry());
    return;
  }
  default: break;
  }
  add_pending(numC(pc_));
  switch (aluop) {
  case 0x0: e().and_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x1: e().eor_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x5: e().adc(hd, hd, hs, true); break;
  case 0x6: e().sbc(hd, hd, hs, true); break;
  case 0x8: e().and_reg(SCRATCH6, hd, hs); set_flags_logical(SCRATCH6, Carry{}); break;
  case 0x9: e().negs(hd, hs); break;
  case 0xA: e().cmp_reg(hd, hs); break;
  case 0xB: e().cmn_reg(hd, hs); break;
  case 0xC: e().orr_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0xE: e().bic_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  default:  e().mvn(hd, hs); set_flags_logical(hd, Carry{}); break;
  }
}

void Translator::thumb_ldr_str(u16 instr, TOp op) {
  Mem m;
  u32 rd;
  int const_nd = -1;
  // effective address -> w1
  switch (op) {
  case TOp::LdrPcRel: {
    rd = (instr >> 8) & 7;
    const u32 addr = ((pc_ + 4) & ~3u) + ((instr & 0xFF) << 2);
    e().mov_imm(SCRATCH1, addr);
    m = Mem::Ld32;
    // ARM9: the literal's page is known, so its N32 cost is baked (see arm_ldr_str).
    if (a9_) { note_dep(addr, mem::Timing::RETIME_DATA); const_nd = cpu_.timing9[addr >> 12][2]; }
    break;
  }
  case TOp::LdrStrReg: {
    const u32 ro = (instr >> 6) & 7, rb = (instr >> 3) & 7;
    rd = instr & 7;
    e().add_reg(SCRATCH1, host_reg(rb), host_reg(ro));
    static const Mem kinds[8] = {Mem::St32, Mem::St16, Mem::St8, Mem::Ld8S, Mem::Ld32, Mem::Ld16, Mem::Ld8, Mem::Ld16S};
    m = kinds[(instr >> 9) & 7];
    break;
  }
  case TOp::LdrStrImm5: {
    const u32 rb = (instr >> 3) & 7, imm = (instr >> 6) & 0x1F;
    rd = instr & 7;
    const bool l = instr & (1 << 11), b = instr & (1 << 12);
    e().add_imm(SCRATCH1, host_reg(rb), b ? imm : imm * 4);
    m = b ? (l ? Mem::Ld8 : Mem::St8) : (l ? Mem::Ld32 : Mem::St32);
    break;
  }
  case TOp::LdrStrHImm5: {
    const u32 rb = (instr >> 3) & 7;
    rd = instr & 7;
    e().add_imm(SCRATCH1, host_reg(rb), ((instr >> 6) & 0x1F) * 2);
    m = (instr & (1 << 11)) ? Mem::Ld16 : Mem::St16;
    break;
  }
  default: {   // LdrStrSpRel
    rd = (instr >> 8) & 7;
    e().add_imm(SCRATCH1, host_reg(13), (instr & 0xFF) * 4);
    m = (instr & (1 << 11)) ? Mem::Ld32 : Mem::St32;
    break;
  }
  }
  if (is_load(m)) emit_single(m, 0, host_reg(rd), false, 0, true, const_nd);
  else emit_single(m, host_reg(rd), 0, false, 0, false);
}

void Translator::translate_thumb(u16 instr) {
  const TOp op = arm::decode_thumb(instr);
  const bool was_prefix = bl_prefix_valid_;
  bl_prefix_valid_ = false;
  if (thumb_needs_fallback(instr, a9_)) {
    const bool always = op == TOp::Swi || op == TOp::Bkpt || op == TOp::Undefined || op == TOp::BxBlx || op == TOp::BlxSuffix;
    emit_fallback(instr, always);
    return;
  }
  switch (op) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    Carry c;
    Operand r = shift_imm(type, amt, Operand{false, host_reg(rs), 0}, live_ & F_C, c, SCRATCH2, SCRATCH3);
    if (r.imm) e().mov_imm(host_reg(rd), r.value); else if (r.reg != host_reg(rd)) e().mov(host_reg(rd), r.reg);
    set_flags_logical(host_reg(rd), c);
    return;
  }
  case TOp::AddSubReg: {
    const u32 rn = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    e().add_sub(instr & (1 << 9), true, host_reg(rd), host_reg(rs), host_reg(rn));
    return;
  }
  case TOp::AddSubImm3: {
    const u32 imm = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    if (instr & (1 << 9)) e().subs_imm(host_reg(rd), host_reg(rs), imm); else e().adds_imm(host_reg(rd), host_reg(rs), imm);
    return;
  }
  case TOp::MovCmpAddSubImm8: {
    const u32 rd = (instr >> 8) & 7, imm = instr & 0xFF;
    add_pending(numC(pc_));
    switch ((instr >> 11) & 3) {
    case 0: e().movz(host_reg(rd), imm); set_flags_logical(host_reg(rd), Carry{}); break;
    case 1: e().cmp_imm(host_reg(rd), imm); break;
    case 2: e().adds_imm(host_reg(rd), host_reg(rd), imm); break;
    default: e().subs_imm(host_reg(rd), host_reg(rd), imm); break;
    }
    return;
  }
  case TOp::Alu: thumb_alu(instr); return;
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), rs = (instr >> 3) & 0xF;
    const Operand b = reg_operand(rs, pc_ + 4);
    switch ((instr >> 8) & 3) {
    case 0:
      if (rd == 15) {
        add_pending(numC(pc_));             // charged before the jump (melonDS T_ADD_HIREG)
        if (b.imm) e().mov_imm(SCRATCH0, pc_ + 4 + b.value); else e().add_imm_any(SCRATCH0, b.reg, pc_ + 4, SCRATCH2);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e().add_imm_any(host_reg(rd), host_reg(rd), b.value, SCRATCH2); else e().add_reg(host_reg(rd), host_reg(rd), b.reg);
      return;
    case 1:
      add_pending(numC(pc_));
      { const u32 a = to_reg(reg_operand(rd, pc_ + 4), SCRATCH4); const u32 bb = to_reg(b, SCRATCH5); e().cmp_reg(a, bb); }
      return;
    case 2:
      if (rd == 15) {
        add_pending(numC(pc_));
        if (b.imm) e().mov_imm(SCRATCH0, b.value); else e().mov(SCRATCH0, b.reg);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e().mov_imm(host_reg(rd), b.value); else e().mov(host_reg(rd), b.reg);
      return;
    default:
      add_pending(numC(pc_));
      return;
    }
  }
  case TOp::BxBlx: {
    const u32 rs = (instr >> 3) & 0xF;
    if (instr & (1 << 7)) {
      if (!a9_) { emit_fallback(instr, true); return; }
      if (rs == 15) e().mov_imm(SCRATCH0, pc_ + 4); else e().mov(SCRATCH0, host_reg(rs));
      e().mov_imm(host_reg(14), (pc_ + 2) | 1);
      emit_branch_indirect(SCRATCH0, true);
      return;
    }
    if (rs == 15) e().mov_imm(SCRATCH0, pc_ + 4); else e().mov(SCRATCH0, host_reg(rs));
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel:
    thumb_ldr_str(instr, op);
    return;
  case TOp::AddPcSp: {
    const u32 rd = (instr >> 8) & 7, imm = (instr & 0xFF) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 11)) e().add_imm(host_reg(rd), host_reg(13), imm);
    else e().mov_imm(host_reg(rd), ((pc_ + 4) & ~3u) + imm);
    return;
  }
  case TOp::AdjustSp: {
    const u32 imm = (instr & 0x7F) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 7)) e().sub_imm(host_reg(13), host_reg(13), imm); else e().add_imm(host_reg(13), host_reg(13), imm);
    return;
  }
  case TOp::PushPop: {
    u32 list = instr & 0xFF;
    const bool pop = instr & (1 << 11), r = instr & (1 << 8);
    if (pop) {
      if (r) list |= 0x8000;
      const u32 n = static_cast<u32>(__builtin_popcount(list));
      if (n == 0) { emit_fallback(instr, false); return; }
      e().mov(SCRATCH1, host_reg(13));
      e().add_imm(SCRATCH7, host_reg(13), n * 4);
      // POP always writes back (r13 is never in a Thumb list).
      emit_block_transfer(instr, list, true, true, 13, a9_, 0);
      return;
    }
    if (r) list |= 0x4000;
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    if (n == 0) { emit_fallback(instr, false); return; }
    e().sub_imm(SCRATCH1, host_reg(13), n * 4);
    e().mov(SCRATCH7, SCRATCH1);
    emit_block_transfer(instr, list, false, true, 13, false, 0);
    return;
  }
  case TOp::StmLdm: {
    const u32 rb = (instr >> 8) & 7, list = instr & 0xFF;
    const bool load = instr & (1 << 11);
    if (list == 0) { emit_fallback(instr, load); return; }
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    e().mov(SCRATCH1, host_reg(rb));
    e().add_imm(SCRATCH7, host_reg(rb), n * 4);
    emit_block_transfer(instr, list, load, true, rb, false, 0);
    return;
  }
  case TOp::BCond: {
    const u32 cond = (instr >> 8) & 0xF;
    const s32 off = static_cast<s8>(instr & 0xFF) * 2;
    flush_pending();
    size_t skip = hot_.b_cond_fwd(invert(static_cast<Cond>(cond)));
    emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true);
    hot_.bind(skip);
    add_pending(numC(pc_));
    emit_branch_static(pc_ + 2, true, false);
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
    e().mov_imm(host_reg(14), bl_prefix_lr_);
    bl_prefix_valid_ = true;
    return;
  }
  case TOp::BlSuffix: case TOp::BlxSuffix: {
    const bool blx = op == TOp::BlxSuffix;
    if (blx && !a9_) { emit_fallback(instr, true); return; }
    const u32 ret = (pc_ + 2) | 1;
    if (was_prefix) {
      u32 target = bl_prefix_lr_ + ((instr & 0x7FF) << 1);
      e().mov_imm(host_reg(14), ret);
      if (blx) emit_branch_static(target & ~3u, false, true);
      else emit_branch_static(target & ~1u, true, true);
      return;
    }
    e().add_imm_any(SCRATCH0, host_reg(14), (instr & 0x7FF) << 1, SCRATCH2);
    e().mov_imm(host_reg(14), ret);
    if (blx) e().and_imm(SCRATCH0, SCRATCH0, ~3u); else e().orr_imm(SCRATCH0, SCRATCH0, 1);
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::Swi: emit_fallback(instr, true); return;
  case TOp::Bkpt: emit_fallback(instr, true); return;
  case TOp::Undefined: emit_fallback(instr, true); return;
  }
}

// ---- block driver ---------------------------------------------------------------------------------------------------

bool Translator::run() {
  const u32 start = key_pc(key_);
  if (!a9_) { t7_ = cpu_.timing7[start >> 15]; code_region7_ = start >> 24; }
  // --cpu-oc bakes main RAM's data costs into every access: not a page the
  // dependency set tracks, so such a block dies on any retime.
  if (rt().cpu_oc) blk_.dep_overflow = true;

  // Decode the straight-line run.
  u32 addr = start;
  for (u32 i = 0; i < MAX_INSTRS; ++i) {
    const u32 raw = fetch(addr);
    instrs_.push_back({addr, raw, F_ALL});
    const bool ends = thumb_ ? thumb_ends_block(static_cast<u16>(raw)) : arm_ends_block(raw, a9_);
    addr += step();
    if (ends) break;
    if (!cpu_.page_table.read_ptr(addr)) break;   // do not walk into unmapped space
  }

  // Backward flag liveness.
  u32 live = F_ALL;
  for (size_t i = instrs_.size(); i-- > 0;) {
    instrs_[i].live_out = live;
    const FlagUse u = thumb_ ? thumb_flag_use(static_cast<u16>(instrs_[i].raw), a9_) : arm_flag_use(instrs_[i].raw, a9_);
    live = u.reads | (live & ~u.writes);
  }

  // Prologue: leave when the budget is exhausted (the exit is cold; the
  // block is always long enough for kill_block's 12-byte redirect).
  if (rt().density) emit_density_bump();
  emit_budget_check(key_);

  u32 end_addr = addr;
  for (size_t i = 0; i < instrs_.size(); ++i) {
    const Instr& in = instrs_[i];
    if (i > 0 && hot_.size() + cold_.size() > BLOCK_LIMIT) { end_addr = in.addr; break; }
    if (hot_.remaining() < 8192 || cold_.remaining() < 4096) return false;
    pc_ = in.addr;
    live_ = in.live_out;
    if (rt().trace) { flush_pending(); emit_trace(in.raw); }
    if (rt().cyclog) { flush_pending(); emit_call2(reinterpret_cast<const void*>(&jit_h_cyclog), in.raw, make_key(in.addr, thumb_)); }
    if (thumb_) translate_thumb(static_cast<u16>(in.raw)); else translate_arm(in.raw);
    rt().stats.instrs_translated++;
    ++dinstrs_;
    if (ended_) break;
    if (rt().strict) {
      // The interpreter tests the budget before every instruction; reproduce
      // that so the two engines interleave identically (verification mode).
      flush_pending();
      emit_budget_check(make_key(in.addr + step(), thumb_));
    }
  }
  blk_.guest_len = end_addr - start;
  if (!ended_) emit_branch_static(end_addr, thumb_, false);
  return finish();
}

} // namespace

bool translate_block(JitCpu& jc, u32 key, Emitter& e, Block& b) {
  Translator t(jc, key, e, b);
  return t.run();
}

} // namespace ds::jit
