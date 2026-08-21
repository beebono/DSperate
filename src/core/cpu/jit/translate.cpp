// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One-pass block translator, ARM and Thumb. Semantics follow the interpreter
// (interp_arm.cpp / interp_thumb.cpp) instruction for instruction, including
// its cycle model (cpu_cycles.h). An instruction that is not translated inline
// runs through `jit_h_fallback`, which *is* the interpreter, so every block is
// complete and exact from the first build; inlining is an optimisation that
// the interpreter-vs-JIT differential test guards.
//
// Scratch register use inside one instruction (see jit_internal.h):
//   w0   loaded value / helper result / branch target
//   w1   effective address (preserved across the cost arithmetic)
//   x2,x3 page-table entry / host base
//   w4-w7 temporaries of the shifter, the cost arithmetic, flag merging
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu_cycles.h"
#include "core/nds.h"

#include <vector>

namespace ds::jit {

namespace {

using arm::AOp;
using arm::TOp;

constexpr u32 MAX_INSTRS = 64;
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
  case AOp::BlxReg: case AOp::Clz:
    return !a9;
  case AOp::Mul: case AOp::Mla:
    return rd == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15 || rn == 15;
  case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    return rd == 15 || rn == 15 || ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15;
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
    break;
  case AOp::Mrs: u.reads = F_ALL; break;
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

// An operand: a host register or a constant (pc-relative values fold).
struct Operand { bool imm; u32 reg; u32 value; };

struct Instr { u32 addr; u32 raw; u32 live_out; };

inline u32 rotr(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

class Translator {
public:
  Translator(JitCpu& jc, u32 key, Emitter& e, Block& b)
      : jc_(jc), cpu_(*jc.ctx), e_(e), blk_(b), key_(key), thumb_(key_thumb(key)), a9_(jc.arm9) {}

  bool run();

private:
  JitCpu& jc_;
  CpuContext& cpu_;
  Emitter& e_;
  Block& blk_;
  const u32 key_;
  const bool thumb_, a9_;
  std::vector<Instr> instrs_;
  u32 pc_ = 0;
  u32 live_ = F_ALL;
  u32 pending_ = 0;
  bool ended_ = false;
  const u8* t7_ = nullptr;
  u32 code_region7_ = 0;
  // Thumb BL pairing: static lr value after a BL prefix at the previous address.
  bool bl_prefix_valid_ = false;
  u32  bl_prefix_lr_ = 0;

  // ---- decoding ------------------------------------------------------------------------
  u32 fetch(u32 addr) {
    if (u8* p = cpu_.page_table.read_ptr(addr)) {
      if (thumb_) { u16 v; std::memcpy(&v, p, 2); return v; }
      u32 v; std::memcpy(&v, p, 4); return v;
    }
    return thumb_ ? cpu_.nds->bus.read16(cpu_.which, addr) : cpu_.nds->bus.read32(cpu_.which, addr);
  }
  static bool arm_ends_block(u32 instr) {
    const u32 cond = instr >> 28;
    if (cond == 0xF) return true;
    switch (arm::decode_arm(instr)) {
    case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Swi: case AOp::Bkpt: case AOp::Undefined:
    case AOp::Mcr: case AOp::MsrReg: case AOp::MsrImm: case AOp::Cdp: case AOp::Ldc: case AOp::Stc: case AOp::Mrc:
      return true;
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
  u32 numC(u32 addr) const {
    if (a9_) {
      const u32 pf = addr + (thumb_ ? 4 : 8);
      if (thumb_ && (pf & 2)) return 0;
      return fetch_cost9(cpu_, pf, false);
    }
    return t7_[thumb_ ? 1 : 3];
  }
  u32 numC_nonseq7() const { return t7_[thumb_ ? 0 : 2]; }
  u32 numC_internal() const { return a9_ ? numC(pc_) : numC_nonseq7(); }   // base cost of a CI instruction
  void add_pending(u32 c) { pending_ += c; }
  void flush_pending() {
    if (!pending_) return;
    e_.sub_imm_any(R_BUDGET, R_BUDGET, pending_, SCRATCH0);
    pending_ = 0;
  }
  size_t budget_exhausted_fwd() {
    e_.sub_imm(SCRATCH0, R_BUDGET, 1);
    return e_.tbnz_fwd(SCRATCH0, 31);
  }

  // ---- flags ---------------------------------------------------------------------------------
  // N,Z from `res` (already computed by a flag-neutral op), C per `carry`, V
  // kept. Only live flags are reproduced. Temporaries: x1, x2, x4.
  void set_flags_logical(u32 res, const Carry& carry, bool res64 = false) {
    const bool need_c = live_ & F_C, need_v = live_ & F_V;
    if (!need_c && !need_v) { e_.tst_reg(res, res, res64); return; }
    const bool keep_c = need_c && carry.kind == CarryKind::Keep;
    if (keep_c || need_v) e_.mrs_nzcv(SCRATCH1);
    e_.tst_reg(res, res, res64);
    e_.mrs_nzcv(SCRATCH2);
    if (keep_c && need_v) {
      e_.ubfx(SCRATCH4, SCRATCH1, 28, 2, true);
      e_.bfi(SCRATCH2, SCRATCH4, 28, 2, true);
    } else {
      if (need_v) { e_.ubfx(SCRATCH4, SCRATCH1, 28, 1, true); e_.bfi(SCRATCH2, SCRATCH4, 28, 1, true); }
      if (keep_c) { e_.ubfx(SCRATCH4, SCRATCH1, 29, 1, true); e_.bfi(SCRATCH2, SCRATCH4, 29, 1, true); }
    }
    if (need_c && carry.kind == CarryKind::Const && carry.value) e_.orr_imm(SCRATCH2, SCRATCH2, 1u << 29);
    if (need_c && carry.kind == CarryKind::Reg) e_.bfi(SCRATCH2, carry.reg, 29, 1, true);
    e_.msr_nzcv(SCRATCH2);
  }

  // ---- operands ---------------------------------------------------------------------------------
  Operand reg_operand(u32 r, u32 pc_value) {
    if (r == 15) return {true, 0, pc_value};
    return {false, host_reg(r), 0};
  }
  u32 to_reg(const Operand& o, u32 scratch) {
    if (!o.imm) return o.reg;
    e_.mov_imm(scratch, o.value);
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
      if (want_carry) { e_.ubfx(cscratch, m, 32 - amt, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e_.lsl_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 1:
      if (amt == 0) { if (want_carry) { e_.lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } return {true, 0, 0}; }
      if (want_carry) { e_.ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e_.lsr_imm(scratch, m, amt);
      return {false, scratch, 0};
    case 2:
      if (amt == 0) { if (want_carry) { e_.lsr_imm(cscratch, m, 31); carry = {CarryKind::Reg, 0, cscratch}; } e_.asr_imm(scratch, m, 31); return {false, scratch, 0}; }
      if (want_carry) { e_.ubfx(cscratch, m, amt - 1, 1); carry = {CarryKind::Reg, 0, cscratch}; }
      e_.asr_imm(scratch, m, amt);
      return {false, scratch, 0};
    default:
      if (amt == 0) {   // RRX
        if (want_carry) { e_.and_imm(cscratch, m, 1); carry = {CarryKind::Reg, 0, cscratch}; }
        e_.cset(SCRATCH7, CS);
        e_.extr(scratch, SCRATCH7, m, 1);
        return {false, scratch, 0};
      }
      e_.ror_imm(scratch, m, amt);
      if (want_carry) { e_.lsr_imm(cscratch, scratch, 31); carry = {CarryKind::Reg, 0, cscratch}; }
      return {false, scratch, 0};
    }
  }

  // Register-amount shifter (amount = rs & 0xFF, ARM rules for >= 32).
  // Result in `scratch`, carry in `cscratch`; uses x4-x7.
  Operand shift_reg(u32 type, u32 rs_host, const Operand& rm, bool want_carry, Carry& carry, u32 scratch, u32 cscratch) {
    carry = {};
    const u32 m = to_reg(rm, scratch);
    const u32 amt = SCRATCH4, tmp = SCRATCH5, old_c = SCRATCH6, wide = SCRATCH7;
    e_.and_imm(amt, rs_host, 0xFF);
    if (want_carry) e_.cset(old_c, CS);
    if (type == 3) {
      e_.rorv(scratch, m, amt);
      if (want_carry) {
        e_.lsr_imm(cscratch, scratch, 31);
        size_t nz = e_.cbnz_fwd(amt);
        e_.mov(cscratch, old_c);
        e_.bind(nz);
        carry = {CarryKind::Reg, 0, cscratch};
      }
      return {false, scratch, 0};
    }
    // amt' = min(amt, 33): 32 keeps its own carry rule, everything above
    // behaves like 33, and a 64-bit shift by amt' then yields result and
    // carry exactly (tmp = amt - max(amt - 33, 0)).
    e_.sub_imm(tmp, amt, 33);
    e_.bic_reg(tmp, tmp, tmp, ASR, 31);
    e_.sub_reg(tmp, amt, tmp);
    switch (type) {
    case 0:
      e_.mov(wide, m);
      e_.lslv(wide, wide, tmp, true);
      if (want_carry) e_.ubfx(cscratch, wide, 32, 1, true);
      e_.mov(scratch, wide);
      break;
    case 1:
      e_.lsl_imm(wide, m, 32, true);
      e_.lsrv(wide, wide, tmp, true);
      if (want_carry) e_.ubfx(cscratch, wide, 31, 1, true);
      e_.lsr_imm(scratch, wide, 32, true);
      break;
    default:
      e_.sbfm(wide, m, 0, 31, true);
      e_.lsl_imm(wide, wide, 32, true);
      e_.asrv(wide, wide, tmp, true);
      if (want_carry) e_.ubfx(cscratch, wide, 31, 1, true);
      e_.lsr_imm(scratch, wide, 32, true);
      break;
    }
    if (want_carry) {
      size_t nz = e_.cbnz_fwd(amt);
      e_.mov(cscratch, old_c);
      e_.bind(nz);
      carry = {CarryKind::Reg, 0, cscratch};
    }
    return {false, scratch, 0};
  }

  // ---- exits, polls, helpers ----------------------------------------------------------------------
  void emit_exit_key(u32 key) {
    e_.movz(SCRATCH0, key & 0xFFFF);
    e_.movk(SCRATCH0, key >> 16, 16);
    e_.b(rt().exit_key);
  }
  // After a helper: leave when the budget is exhausted, an alert is raised,
  // or an IRQ is pending and unmasked (the C side takes it).
  void emit_poll(u32 next_key, bool r15_in_ctx) {
    size_t exhausted = budget_exhausted_fwd();
    e_.ldr_w(SCRATCH0, R_CTX, OFF_ALERTS);
    size_t alert = e_.cbnz_fwd(SCRATCH0);
    e_.ldr_w(SCRATCH0, R_CTX, OFF_IRQ);
    size_t no_irq = e_.cbz_fwd(SCRATCH0);
    e_.ldr_w(SCRATCH0, R_CTX, OFF_CPSR);
    size_t masked = e_.tbnz_fwd(SCRATCH0, 7);
    e_.bind(exhausted);
    e_.bind(alert);
    if (r15_in_ctx) e_.b(rt().exit_r15);
    else emit_exit_key(next_key);
    e_.bind(no_irq);
    e_.bind(masked);
  }
  void emit_call_full(const void* fn, u32 instr, u32 key) {
    e_.mov(SCRATCH0, R_CTX, true);
    e_.mov_imm(SCRATCH1, instr);
    e_.mov_imm(SCRATCH2, key);
    e_.mov_imm64(R_FN, reinterpret_cast<u64>(fn));
    e_.bl(rt().call_full);
  }
  void emit_dispatch_from_ctx() {
    e_.ldr_w(SCRATCH0, R_CTX, off_reg(15));
    e_.ldr_w(SCRATCH1, R_CTX, OFF_CPSR);
    e_.ubfx(SCRATCH2, SCRATCH1, 5, 1);
    e_.sub_imm(SCRATCH0, SCRATCH0, 8);
    e_.add_reg(SCRATCH0, SCRATCH0, SCRATCH2, LSL, 2);
    e_.orr_reg(SCRATCH0, SCRATCH0, SCRATCH2);
    e_.b(jc_.dispatch);
  }
  // Run this instruction through the interpreter (it evaluates the condition
  // itself and charges its own cycles), then poll; dispatch if it jumped.
  void emit_fallback(u32 instr, bool may_jump, bool always_jumps) {
    flush_pending();
    emit_call_full(reinterpret_cast<const void*>(&jit_h_fallback), instr, make_key(pc_, thumb_));
    const u32 next_key = make_key(pc_ + (thumb_ ? 2 : 4), thumb_);
    if (may_jump) {
      size_t not_jumped = 0;
      if (!always_jumps) not_jumped = e_.cbz_fwd(SCRATCH0);
      emit_poll(0, true);
      emit_dispatch_from_ctx();
      if (always_jumps) { ended_ = true; return; }
      e_.bind(not_jumped);
    }
    emit_poll(next_key, false);
  }
  void emit_trace(u32 instr) {
    emit_call_full(reinterpret_cast<const void*>(&jit_h_trace), instr, make_key(pc_, thumb_));
  }

  // ---- branches ----------------------------------------------------------------------------------
  void emit_branch_static(u32 target, bool to_thumb, bool refill) {
    if (refill) add_pending(refill_cycles(cpu_, target, to_thumb));
    flush_pending();
    if (to_thumb != thumb_) {
      e_.ldr_w(SCRATCH0, R_CTX, OFF_CPSR);
      e_.eor_imm(SCRATCH0, SCRATCH0, 0x20);
      e_.str_w(SCRATCH0, R_CTX, OFF_CPSR);
    }
    const u32 key = make_key(target, to_thumb);
    e_.movz(SCRATCH0, key & 0xFFFF);
    e_.movk(SCRATCH0, key >> 16, 16);
    e_.bl(jc_.link);
    ended_ = true;
  }
  // Indirect branch; `wtarget` holds the address. With `interwork` bit 0
  // selects the state, otherwise the current state is kept.
  void emit_branch_indirect(u32 wtarget, bool interwork, bool cdi = false) {
    flush_pending();
    if (wtarget != SCRATCH0) e_.mov(SCRATCH0, wtarget);
    if (!interwork) {
      if (thumb_) e_.orr_imm(SCRATCH0, SCRATCH0, 1);
      else e_.and_imm(SCRATCH0, SCRATCH0, ~1u);
    }
    e_.b(cdi ? jc_.branch_indirect_cdi : jc_.branch_indirect);
    ended_ = true;
  }

  // ---- memory ---------------------------------------------------------------------------------------
  enum class Mem { Ld32, Ld16, Ld8, Ld16S, Ld8S, St32, St16, St8 };
  static bool is_load(Mem m) { return m <= Mem::Ld8S; }
  static bool is_word(Mem m) { return m == Mem::Ld32 || m == Mem::St32; }

  struct SlowPath { std::vector<size_t> fail; };

  // Page-table lookup for w1. On success x3 = pre-biased host base. Failure
  // branches are collected in `sp`. Clobbers x2, x3.
  void emit_page_lookup(u32 waddr, bool store, SlowPath& sp) {
    e_.lsr_imm(SCRATCH2, waddr, mem::PAGE_SHIFT);
    e_.ldr_x_reg(SCRATCH2, R_PT, SCRATCH2, true, true);
    if (store) {
      e_.lsr_imm(SCRATCH3, SCRATCH2, 62, true);
      sp.fail.push_back(e_.cbnz_fwd(SCRATCH3));
    }
    e_.lsl_imm(SCRATCH3, SCRATCH2, 2, true);
    sp.fail.push_back(e_.cbz_fwd(SCRATCH3, true));
  }
  // One access at [x3 + w1]; loads leave the value in w0. Clobbers x4, x5.
  void emit_access(Mem m, u32 waddr, u32 wdata) {
    switch (m) {
    case Mem::Ld32:
      e_.and_imm(SCRATCH4, waddr, ~3u);
      e_.ldr_w_reg(SCRATCH0, SCRATCH3, SCRATCH4);
      e_.lsl_imm(SCRATCH5, waddr, 3);
      e_.rorv(SCRATCH0, SCRATCH0, SCRATCH5);
      break;
    case Mem::Ld16:
      e_.and_imm(SCRATCH4, waddr, ~1u);
      e_.ldrh_reg(SCRATCH0, SCRATCH3, SCRATCH4);
      if (!a9_) { e_.ubfiz(SCRATCH5, waddr, 3, 1); e_.rorv(SCRATCH0, SCRATCH0, SCRATCH5); }
      break;
    case Mem::Ld8:  e_.ldrb_reg(SCRATCH0, SCRATCH3, waddr); break;
    case Mem::Ld8S: e_.ldrsb_w_reg(SCRATCH0, SCRATCH3, waddr); break;
    case Mem::Ld16S:
      if (a9_) { e_.and_imm(SCRATCH4, waddr, ~1u); e_.ldrsh_w_reg(SCRATCH0, SCRATCH3, SCRATCH4); }
      else {
        size_t odd = e_.tbnz_fwd(waddr, 0);
        e_.ldrsh_w_reg(SCRATCH0, SCRATCH3, waddr);
        size_t j = e_.b_fwd();
        e_.bind(odd);
        e_.ldrsb_w_reg(SCRATCH0, SCRATCH3, waddr);
        e_.bind(j);
      }
      break;
    case Mem::St32: e_.and_imm(SCRATCH4, waddr, ~3u); e_.str_w_reg(wdata, SCRATCH3, SCRATCH4); break;
    case Mem::St16: e_.and_imm(SCRATCH4, waddr, ~1u); e_.strh_reg(wdata, SCRATCH3, SCRATCH4); break;
    case Mem::St8:  e_.strb_reg(wdata, SCRATCH3, waddr); break;
    }
  }
  // Data cost of one access at `waddr` into `wcost`.
  void emit_data_cost(u32 waddr, u32 wcost, bool word, bool seq) {
    const u32 k = a9_ ? (seq ? 3 : (word ? 2 : 1)) : (seq ? (word ? 3 : 1) : (word ? 2 : 0));
    e_.lsr_imm(wcost, waddr, a9_ ? 12 : 15);
    e_.add_reg(wcost, R_TIM, wcost, LSL, 2, true);
    e_.ldrb(wcost, wcost, k);
  }
  // wd = max(wa, wb) without flags: wa + max(wb - wa, 0)
  void emit_max(u32 wd, u32 wa, u32 wb, u32 tmp) {
    e_.sub_reg(tmp, wb, wa);
    e_.bic_reg(tmp, tmp, tmp, ASR, 31);
    e_.add_reg(wd, wa, tmp);
  }
  // Charge a CD / CDI instruction: data cost in `wd`, address in `waddr`
  // (ARM7 main-RAM rule). Uses x4-x7.
  void emit_charge_data(u32 wd, u32 waddr, bool cdi) {
    flush_pending();
    if (rt().fastcost) {   // DS_JIT_FASTCOST: measurement knob, inexact: numC + numD
      e_.add_imm(SCRATCH5, wd, a9_ ? numC(pc_) : numC_nonseq7());
      e_.sub_reg(R_BUDGET, R_BUDGET, SCRATCH5);
      return;
    }
    if (a9_) {
      const u32 nc = numC(pc_);
      e_.mov_imm(SCRATCH6, nc);
      emit_max(SCRATCH5, SCRATCH6, wd, SCRATCH7);                       // max(nc, nd)
      if (nc >= 6) e_.add_imm(SCRATCH6, wd, nc - 6); else e_.sub_imm(SCRATCH6, wd, 6 - nc);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH6, SCRATCH7);                 // max(.., nc + nd - 6)
      e_.sub_reg(R_BUDGET, R_BUDGET, SCRATCH5);
      return;
    }
    const u32 nc = numC_nonseq7();
    const bool code_main = code_region7_ == 0x02;
    e_.lsr_imm(SCRATCH6, waddr, 24);
    e_.eor_imm(SCRATCH6, SCRATCH6, 2);
    size_t not_main = e_.cbnz_fwd(SCRATCH6);
    if (code_main) {
      e_.add_imm(SCRATCH5, wd, nc);
    } else {
      const u32 ncx = cdi ? nc + 1 : nc;
      e_.mov_imm(SCRATCH6, ncx);
      emit_max(SCRATCH5, SCRATCH6, wd, SCRATCH7);
      if (ncx >= 3) e_.add_imm(SCRATCH6, wd, ncx - 3); else e_.sub_imm(SCRATCH6, wd, 3 - ncx);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH6, SCRATCH7);
    }
    size_t done = e_.b_fwd();
    e_.bind(not_main);
    if (code_main) {
      if (cdi) e_.add_imm(SCRATCH4, wd, 1); else e_.mov(SCRATCH4, wd);
      e_.mov_imm(SCRATCH6, nc);
      emit_max(SCRATCH5, SCRATCH6, SCRATCH4, SCRATCH7);
      if (nc >= 3) e_.add_imm(SCRATCH6, SCRATCH4, nc - 3); else e_.sub_imm(SCRATCH6, SCRATCH4, 3 - nc);
      emit_max(SCRATCH5, SCRATCH5, SCRATCH6, SCRATCH7);
    } else {
      e_.add_imm(SCRATCH5, wd, nc + (cdi ? 1 : 0));
    }
    e_.bind(done);
    e_.sub_reg(R_BUDGET, R_BUDGET, SCRATCH5);
  }
  // Single access cost + charge.
  void emit_single_cost(u32 waddr, bool word, bool cdi) {
    emit_data_cost(waddr, SCRATCH2, word, false);
    emit_charge_data(SCRATCH2, waddr, cdi);
  }
  // Close a fast path: jump over the slow path, bind the failures, emit the
  // interpreter fallback for the whole instruction, join.
  void emit_slow_join(SlowPath& sp, u32 instr, bool may_jump, bool always_jumps) {
    size_t done = e_.b_fwd();
    for (size_t f : sp.fail) e_.bind(f);
    emit_fallback(instr, may_jump, always_jumps);
    e_.bind(done);
    ended_ = false;       // the fast path continues the block
  }

  // Multi-register transfer, fast path in one 2 KB page. `list` is 16 bits;
  // `wstart` (w1) = lowest address, `wwb` (w7) = writeback value.
  // `rn` = base register for the STM "Rn in list" rule (or 16 for none).
  void emit_block_transfer(u32 instr, u32 list, bool load, bool writeback, u32 rn, bool interwork_pc, u32 pc_store_value) {
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    SlowPath sp;
    e_.add_imm(SCRATCH2, SCRATCH1, n * 4 - 1);
    e_.eor_reg(SCRATCH2, SCRATCH2, SCRATCH1);
    e_.lsr_imm(SCRATCH2, SCRATCH2, mem::PAGE_SHIFT);
    sp.fail.push_back(e_.cbnz_fwd(SCRATCH2));
    emit_page_lookup(SCRATCH1, !load, sp);
    e_.and_imm(SCRATCH2, SCRATCH1, ~3u);          // each word access is aligned; the base may not be (Thumb)
    e_.add_uxtw(SCRATCH3, SCRATCH3, SCRATCH2);
    u32 k = 0;
    bool first = true;
    const bool pc_in_list = (list & 0x8000) != 0;
    if (load) {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        e_.ldr_w(i == 15 ? SCRATCH0 : host_reg(i), SCRATCH3, 4 * k);
        ++k;
      }
      if (writeback && !(list & (1u << rn))) e_.mov(host_reg(rn), SCRATCH7);
    } else {
      for (u32 i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        u32 src;
        if (i == 15) { e_.mov_imm(SCRATCH0, pc_store_value); src = SCRATCH0; }
        else if (i == rn && !first && writeback) src = SCRATCH7;
        else src = host_reg(i);
        e_.str_w(src, SCRATCH3, 4 * k);
        ++k; first = false;
      }
      if (writeback) e_.mov(host_reg(rn), SCRATCH7);
    }
    // cost: N + (n - 1) S from the page's entry
    emit_data_cost(SCRATCH1, SCRATCH4, true, false);
    if (n > 1) {
      emit_data_cost(SCRATCH1, SCRATCH5, true, true);
      e_.mov_imm(SCRATCH6, n - 1);
      e_.madd(SCRATCH4, SCRATCH5, SCRATCH6, SCRATCH4);
    }
    if (load && pc_in_list) {
      // The interpreter charges the CDI cost after the jump: the stub does it
      // from the new pc/state (w1 = numD, w2 = data address for the ARM7 rule).
      e_.mov(SCRATCH2, SCRATCH1);
      e_.mov(SCRATCH1, SCRATCH4);
      emit_branch_indirect(SCRATCH0, interwork_pc, true);
      for (size_t f : sp.fail) e_.bind(f);
      emit_fallback(instr, true, true);
      ended_ = true;
      return;
    }
    emit_charge_data(SCRATCH4, SCRATCH1, load);
    emit_slow_join(sp, instr, false, false);
  }

  // ---- drivers -------------------------------------------------------------------------------------------
  void translate_arm(u32 instr);
  void translate_thumb(u16 instr);
  void arm_data_processing(u32 instr, AOp op);
  void arm_multiply(u32 instr, AOp op);
  void arm_ldr_str(u32 instr, AOp op);
  void arm_ldr_str_h(u32 instr, AOp op);
  void arm_ldm_stm(u32 instr, bool load);
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
  if (rd == 15 && !test) { emit_fallback(instr, true, true); return; }
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

  const u32 dst = test ? SCRATCH6 : host_reg(rd);
  const u32 a = to_reg(rn_v, SCRATCH4);
  auto b_reg = [&]() { return to_reg(op2, SCRATCH5); };

  switch (opcode) {
  case 0x0: case 0x8:
    if (op2.imm) { if (!e_.and_imm(dst, a, op2.value)) e_.and_reg(dst, a, b_reg()); }
    else e_.and_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x1: case 0x9:
    if (op2.imm) { if (!e_.eor_imm(dst, a, op2.value)) e_.eor_reg(dst, a, b_reg()); }
    else e_.eor_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xC:
    if (op2.imm) { if (!e_.orr_imm(dst, a, op2.value)) e_.orr_reg(dst, a, b_reg()); }
    else e_.orr_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xE:
    if (op2.imm) { if (!e_.and_imm(dst, a, ~op2.value)) e_.bic_reg(dst, a, b_reg()); }
    else e_.bic_reg(dst, a, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xD:
    if (op2.imm) e_.mov_imm(dst, op2.value); else if (op2.reg != dst) e_.mov(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0xF:
    if (op2.imm) e_.mov_imm(dst, ~op2.value); else e_.mvn(dst, op2.reg);
    if (s) set_flags_logical(dst, carry);
    break;
  case 0x2: case 0xA: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e_.sub_imm_any(d, a, op2.value, SCRATCH5, s); else e_.add_sub(true, s, d, a, op2.reg);
    break;
  }
  case 0x4: case 0xB: {
    const u32 d = test ? ZR : dst;
    if (op2.imm) e_.add_imm_any(d, a, op2.value, SCRATCH5, s); else e_.add_sub(false, s, d, a, op2.reg);
    break;
  }
  case 0x3:
    if (op2.imm && op2.value == 0) { if (s) e_.negs(dst, a); else e_.neg(dst, a); }
    else e_.add_sub(true, s, dst, b_reg(), a);
    break;
  case 0x5: e_.adc(dst, a, b_reg(), s); break;
  case 0x6: e_.sbc(dst, a, b_reg(), s); break;
  case 0x7: e_.sbc(dst, b_reg(), a, s); break;
  }
}

void Translator::emit_mul_cycles7(u32 wrs, bool signed_op, u32 extra) {
  // 1..4 internal cycles from the magnitude of rs (signed: of rs ^ (rs >> 31)).
  if (signed_op) e_.eor_reg(SCRATCH6, wrs, wrs, ASR, 31); else e_.mov(SCRATCH6, wrs);
  e_.clz(SCRATCH6, SCRATCH6);
  e_.lsr_imm(SCRATCH6, SCRATCH6, 3);          // leading zero bytes 0..4
  e_.lsl_imm(SCRATCH6, SCRATCH6, 2);
  e_.mov_imm(SCRATCH7, 0x11234);              // nibble table: 4,3,2,1,1
  e_.lsrv(SCRATCH7, SCRATCH7, SCRATCH6);
  e_.and_imm(SCRATCH7, SCRATCH7, 0xF);
  if (extra) e_.add_imm(SCRATCH7, SCRATCH7, extra);
  e_.sub_reg(R_BUDGET, R_BUDGET, SCRATCH7);
}

void Translator::arm_multiply(u32 instr, AOp op) {
  const bool s = instr & (1u << 20);
  const u32 rs = (instr >> 8) & 0xF, rm = instr & 0xF;
  if (rs == 15 || rm == 15) { emit_fallback(instr, false, false); return; }
  const u32 hrs = host_reg(rs), hrm = host_reg(rm);
  if (op == AOp::Mul || op == AOp::Mla) {
    const u32 rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF;
    if (rd == 15 || rn == 15) { emit_fallback(instr, false, false); return; }
    if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, true, op == AOp::Mla ? 1 : 0); }
    if (op == AOp::Mla) e_.madd(host_reg(rd), hrm, hrs, host_reg(rn)); else e_.mul(host_reg(rd), hrm, hrs);
    if (s) set_flags_logical(host_reg(rd), mul_carry());
    return;
  }
  const u32 rdhi = (instr >> 16) & 0xF, rdlo = (instr >> 12) & 0xF;
  if (rdhi == 15 || rdlo == 15) { emit_fallback(instr, false, false); return; }
  const bool sgn = op == AOp::Smull || op == AOp::Smlal;
  const bool acc = op == AOp::Umlal || op == AOp::Smlal;
  if (a9_) add_pending(numC(pc_) + (s ? 3 : 1));
  else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hrs, sgn, 1); }
  if (acc) {
    e_.mov(SCRATCH0, host_reg(rdlo));
    e_.bfi(SCRATCH0, host_reg(rdhi), 32, 32, true);
    if (sgn) e_.smaddl(SCRATCH0, hrm, hrs, SCRATCH0); else e_.umaddl(SCRATCH0, hrm, hrs, SCRATCH0);
  } else {
    if (sgn) e_.smull(SCRATCH0, hrm, hrs); else e_.umull(SCRATCH0, hrm, hrs);
  }
  e_.mov(host_reg(rdlo), SCRATCH0);
  e_.lsr_imm(host_reg(rdhi), SCRATCH0, 32, true);
  if (s) set_flags_logical(SCRATCH0, mul_carry(), true);
}

void Translator::arm_ldr_str(u32 instr, AOp op) {
  const bool l = instr & (1u << 20), b = instr & (1u << 22);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const bool writeback = !p || w;
  if ((l && rd == 15) || (rn == 15 && writeback) || (op == AOp::LdrStrReg && (instr & 0xF) == 15)) {
    emit_fallback(instr, l && rd == 15, l && rd == 15);
    return;
  }
  Operand off;
  if (op == AOp::LdrStrImm) off = {true, 0, instr & 0xFFF};
  else { Carry c; off = shift_imm((instr >> 5) & 3, (instr >> 7) & 0x1F, reg_operand(instr & 0xF, pc_ + 8), false, c, SCRATCH2, SCRATCH3); }
  const u32 hb = to_reg(reg_operand(rn, pc_ + 8), SCRATCH6);
  if (off.imm) { if (u) e_.add_imm_any(SCRATCH7, hb, off.value, SCRATCH5); else e_.sub_imm_any(SCRATCH7, hb, off.value, SCRATCH5); }
  else { if (u) e_.add_reg(SCRATCH7, hb, off.reg); else e_.sub_reg(SCRATCH7, hb, off.reg); }
  if (p) e_.mov(SCRATCH1, SCRATCH7); else e_.mov(SCRATCH1, hb);

  SlowPath sp;
  if (l) {
    const Mem m = b ? Mem::Ld8 : Mem::Ld32;
    emit_page_lookup(SCRATCH1, false, sp);
    emit_access(m, SCRATCH1, 0);
    if (writeback) e_.mov(host_reg(rn), SCRATCH7);
    e_.mov(host_reg(rd), SCRATCH0);
    emit_single_cost(SCRATCH1, !b, true);
  } else {
    const Mem m = b ? Mem::St8 : Mem::St32;
    u32 data = host_reg(rd);
    if (rd == 15) { e_.mov_imm(SCRATCH0, pc_ + 12); data = SCRATCH0; }
    emit_page_lookup(SCRATCH1, true, sp);
    emit_access(m, SCRATCH1, data);
    if (writeback) e_.mov(host_reg(rn), SCRATCH7);
    emit_single_cost(SCRATCH1, !b, false);
  }
  emit_slow_join(sp, instr, false, false);
}

void Translator::arm_ldr_str_h(u32 instr, AOp op) {
  const bool l = instr & (1u << 20);
  const bool p = instr & (1u << 24), u = instr & (1u << 23), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 sh = (instr >> 5) & 3;
  const bool writeback = !p || w;
  // LDRD/STRD, pc forms and writeback on pc: interpreter.
  if ((!l && sh != 1) || rd == 15 || (rn == 15 && writeback) || (op == AOp::LdrStrHReg && (instr & 0xF) == 15)) {
    emit_fallback(instr, l && rd == 15, l && rd == 15);
    return;
  }
  Operand off;
  if (op == AOp::LdrStrHImm) off = {true, 0, ((instr >> 4) & 0xF0) | (instr & 0xF)};
  else off = {false, host_reg(instr & 0xF), 0};
  const u32 hb = to_reg(reg_operand(rn, pc_ + 8), SCRATCH6);
  if (off.imm) { if (u) e_.add_imm_any(SCRATCH7, hb, off.value, SCRATCH5); else e_.sub_imm_any(SCRATCH7, hb, off.value, SCRATCH5); }
  else { if (u) e_.add_reg(SCRATCH7, hb, off.reg); else e_.sub_reg(SCRATCH7, hb, off.reg); }
  if (p) e_.mov(SCRATCH1, SCRATCH7); else e_.mov(SCRATCH1, hb);

  SlowPath sp;
  if (l) {
    const Mem m = sh == 1 ? Mem::Ld16 : sh == 2 ? Mem::Ld8S : Mem::Ld16S;
    emit_page_lookup(SCRATCH1, false, sp);
    emit_access(m, SCRATCH1, 0);
    if (writeback) e_.mov(host_reg(rn), SCRATCH7);
    e_.mov(host_reg(rd), SCRATCH0);
    emit_single_cost(SCRATCH1, false, true);
  } else {
    emit_page_lookup(SCRATCH1, true, sp);
    emit_access(Mem::St16, SCRATCH1, host_reg(rd));
    if (writeback) e_.mov(host_reg(rn), SCRATCH7);
    emit_single_cost(SCRATCH1, false, false);
  }
  emit_slow_join(sp, instr, false, false);
}

void Translator::arm_ldm_stm(u32 instr, bool load) {
  const bool p = instr & (1u << 24), u = instr & (1u << 23), s = instr & (1u << 22), w = instr & (1u << 21);
  const u32 rn = (instr >> 16) & 0xF;
  const u32 list = instr & 0xFFFF;
  const u32 n = static_cast<u32>(__builtin_popcount(list));
  const bool pc_in_list = (list & 0x8000) != 0;
  if (s || n == 0 || rn == 15) { emit_fallback(instr, load && pc_in_list, load && pc_in_list); return; }
  const u32 hb = host_reg(rn);
  // start address -> w1, writeback value -> w7
  if (u) {
    if (p) e_.add_imm(SCRATCH1, hb, 4); else e_.mov(SCRATCH1, hb);
    e_.add_imm(SCRATCH7, hb, n * 4);
  } else {
    if (p) e_.sub_imm(SCRATCH1, hb, n * 4); else e_.sub_imm(SCRATCH1, hb, n * 4 - 4);
    e_.sub_imm(SCRATCH7, hb, n * 4);
  }
  e_.and_imm(SCRATCH1, SCRATCH1, ~3u);
  emit_block_transfer(instr, list, load, w, rn, a9_, pc_ + 12);
}

void Translator::translate_arm(u32 instr) {
  const u32 cond = instr >> 28;
  if (cond == 0xF) {
    if (((instr >> 25) & 7) == 5 && a9_) {       // BLX imm
      s32 off = static_cast<s32>(instr << 8) >> 6;
      off |= (instr >> 23) & 2;
      e_.mov_imm(host_reg(14), pc_ + 4);
      emit_branch_static((pc_ + 8 + static_cast<u32>(off)) & ~1u, true, true);
      return;
    }
    if (((instr >> 24) & 0xF7) == 0x55) { add_pending(numC(pc_)); return; }   // PLD
    emit_fallback(instr, true, true);
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
    emit_fallback(instr, true, always);
    return;
  }

  size_t skip = 0;
  const bool conditional = cond != 0xE;
  if (conditional) {
    flush_pending();
    skip = e_.b_cond_fwd(invert(static_cast<Cond>(cond)));
  }

  switch (op) {
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift:
    arm_data_processing(instr, op);
    break;
  case AOp::Mrs: {
    const u32 rd = (instr >> 12) & 0xF;
    add_pending(numC(pc_));
    if (rd == 15) break;
    if (instr & (1u << 22)) e_.ldr_w(host_reg(rd), R_CTX, OFF_SPSR);
    else {
      e_.mrs_nzcv(SCRATCH1);
      e_.ldr_w(SCRATCH2, R_CTX, OFF_CPSR);
      e_.and_imm(SCRATCH2, SCRATCH2, 0x0FFFFFFF);
      e_.orr_reg(host_reg(rd), SCRATCH2, SCRATCH1);
    }
    break;
  }
  case AOp::B: case AOp::Bl: {
    const s32 off = static_cast<s32>(instr << 8) >> 6;
    if (op == AOp::Bl) e_.mov_imm(host_reg(14), pc_ + 4);
    emit_branch_static(pc_ + 8 + static_cast<u32>(off), false, true);
    break;
  }
  case AOp::Bx: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e_.mov_imm(SCRATCH0, pc_ + 8); else e_.mov(SCRATCH0, host_reg(rm));
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::BlxReg: {
    const u32 rm = instr & 0xF;
    if (rm == 15) e_.mov_imm(SCRATCH0, pc_ + 8); else e_.mov(SCRATCH0, host_reg(rm));
    e_.mov_imm(host_reg(14), pc_ + 4);
    emit_branch_indirect(SCRATCH0, true);
    break;
  }
  case AOp::Clz: {
    const u32 rd = (instr >> 12) & 0xF, rm = instr & 0xF;
    add_pending(numC(pc_));
    if (rd == 15 || rm == 15) break;
    e_.clz(host_reg(rd), host_reg(rm));
    break;
  }
  case AOp::Pld: add_pending(numC(pc_)); break;
  case AOp::Mul: case AOp::Mla: case AOp::Umull: case AOp::Umlal: case AOp::Smull: case AOp::Smlal:
    arm_multiply(instr, op);
    break;
  case AOp::LdrStrImm: case AOp::LdrStrReg: arm_ldr_str(instr, op); break;
  case AOp::LdrStrHImm: case AOp::LdrStrHReg: arm_ldr_str_h(instr, op); break;
  case AOp::Ldm: arm_ldm_stm(instr, true); break;
  case AOp::Stm: arm_ldm_stm(instr, false); break;
  default: break;
  }

  if (conditional) {
    if (ended_) {
      e_.bind(skip);
      add_pending(numC(pc_));
      emit_branch_static(pc_ + 4, false, false);
    } else {
      flush_pending();
      size_t join = e_.b_fwd();
      e_.bind(skip);
      add_pending(numC(pc_));
      flush_pending();
      e_.bind(join);
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
    e_.mov(hd, r.reg);
    set_flags_logical(hd, c);
    return;
  }
  case 0xD: {   // MUL
    if (a9_) add_pending(numC(pc_) + 3);
    else { add_pending(numC_nonseq7()); flush_pending(); emit_mul_cycles7(hd, true, 0); }
    e_.mul(hd, hd, hs);
    set_flags_logical(hd, mul_carry());
    return;
  }
  default: break;
  }
  add_pending(numC(pc_));
  switch (aluop) {
  case 0x0: e_.and_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x1: e_.eor_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0x5: e_.adc(hd, hd, hs, true); break;
  case 0x6: e_.sbc(hd, hd, hs, true); break;
  case 0x8: e_.and_reg(SCRATCH6, hd, hs); set_flags_logical(SCRATCH6, Carry{}); break;
  case 0x9: e_.negs(hd, hs); break;
  case 0xA: e_.cmp_reg(hd, hs); break;
  case 0xB: e_.cmn_reg(hd, hs); break;
  case 0xC: e_.orr_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  case 0xE: e_.bic_reg(hd, hd, hs); set_flags_logical(hd, Carry{}); break;
  default:  e_.mvn(hd, hs); set_flags_logical(hd, Carry{}); break;
  }
}

void Translator::thumb_ldr_str(u16 instr, TOp op) {
  Mem m;
  u32 rd;
  // effective address -> w1
  switch (op) {
  case TOp::LdrPcRel: {
    rd = (instr >> 8) & 7;
    e_.mov_imm(SCRATCH1, ((pc_ + 4) & ~3u) + ((instr & 0xFF) << 2));
    m = Mem::Ld32;
    break;
  }
  case TOp::LdrStrReg: {
    const u32 ro = (instr >> 6) & 7, rb = (instr >> 3) & 7;
    rd = instr & 7;
    e_.add_reg(SCRATCH1, host_reg(rb), host_reg(ro));
    static const Mem kinds[8] = {Mem::St32, Mem::St16, Mem::St8, Mem::Ld8S, Mem::Ld32, Mem::Ld16, Mem::Ld8, Mem::Ld16S};
    m = kinds[(instr >> 9) & 7];
    break;
  }
  case TOp::LdrStrImm5: {
    const u32 rb = (instr >> 3) & 7, imm = (instr >> 6) & 0x1F;
    rd = instr & 7;
    const bool l = instr & (1 << 11), b = instr & (1 << 12);
    e_.add_imm(SCRATCH1, host_reg(rb), b ? imm : imm * 4);
    m = b ? (l ? Mem::Ld8 : Mem::St8) : (l ? Mem::Ld32 : Mem::St32);
    break;
  }
  case TOp::LdrStrHImm5: {
    const u32 rb = (instr >> 3) & 7;
    rd = instr & 7;
    e_.add_imm(SCRATCH1, host_reg(rb), ((instr >> 6) & 0x1F) * 2);
    m = (instr & (1 << 11)) ? Mem::Ld16 : Mem::St16;
    break;
  }
  default: {   // LdrStrSpRel
    rd = (instr >> 8) & 7;
    e_.add_imm(SCRATCH1, host_reg(13), (instr & 0xFF) * 4);
    m = (instr & (1 << 11)) ? Mem::Ld32 : Mem::St32;
    break;
  }
  }
  SlowPath sp;
  if (is_load(m)) {
    emit_page_lookup(SCRATCH1, false, sp);
    emit_access(m, SCRATCH1, 0);
    e_.mov(host_reg(rd), SCRATCH0);
    emit_single_cost(SCRATCH1, is_word(m), true);
  } else {
    emit_page_lookup(SCRATCH1, true, sp);
    emit_access(m, SCRATCH1, host_reg(rd));
    emit_single_cost(SCRATCH1, is_word(m), false);
  }
  emit_slow_join(sp, instr, false, false);
}

void Translator::translate_thumb(u16 instr) {
  const TOp op = arm::decode_thumb(instr);
  const bool was_prefix = bl_prefix_valid_;
  bl_prefix_valid_ = false;
  if (thumb_needs_fallback(instr, a9_)) {
    const bool always = op == TOp::Swi || op == TOp::Bkpt || op == TOp::Undefined || op == TOp::BxBlx || op == TOp::BlxSuffix;
    emit_fallback(instr, true, always);
    return;
  }
  switch (op) {
  case TOp::ShiftImm: {
    const u32 type = (instr >> 11) & 3, amt = (instr >> 6) & 0x1F, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    Carry c;
    Operand r = shift_imm(type, amt, Operand{false, host_reg(rs), 0}, live_ & F_C, c, SCRATCH2, SCRATCH3);
    if (r.imm) e_.mov_imm(host_reg(rd), r.value); else if (r.reg != host_reg(rd)) e_.mov(host_reg(rd), r.reg);
    set_flags_logical(host_reg(rd), c);
    return;
  }
  case TOp::AddSubReg: {
    const u32 rn = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    e_.add_sub(instr & (1 << 9), true, host_reg(rd), host_reg(rs), host_reg(rn));
    return;
  }
  case TOp::AddSubImm3: {
    const u32 imm = (instr >> 6) & 7, rs = (instr >> 3) & 7, rd = instr & 7;
    add_pending(numC(pc_));
    if (instr & (1 << 9)) e_.subs_imm(host_reg(rd), host_reg(rs), imm); else e_.adds_imm(host_reg(rd), host_reg(rs), imm);
    return;
  }
  case TOp::MovCmpAddSubImm8: {
    const u32 rd = (instr >> 8) & 7, imm = instr & 0xFF;
    add_pending(numC(pc_));
    switch ((instr >> 11) & 3) {
    case 0: e_.movz(host_reg(rd), imm); set_flags_logical(host_reg(rd), Carry{}); break;
    case 1: e_.cmp_imm(host_reg(rd), imm); break;
    case 2: e_.adds_imm(host_reg(rd), host_reg(rd), imm); break;
    default: e_.subs_imm(host_reg(rd), host_reg(rd), imm); break;
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
        if (b.imm) e_.mov_imm(SCRATCH0, pc_ + 4 + b.value); else e_.add_imm_any(SCRATCH0, b.reg, pc_ + 4, SCRATCH2);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e_.add_imm_any(host_reg(rd), host_reg(rd), b.value, SCRATCH2); else e_.add_reg(host_reg(rd), host_reg(rd), b.reg);
      return;
    case 1:
      add_pending(numC(pc_));
      { const u32 a = to_reg(reg_operand(rd, pc_ + 4), SCRATCH4); const u32 bb = to_reg(b, SCRATCH5); e_.cmp_reg(a, bb); }
      return;
    case 2:
      if (rd == 15) {
        add_pending(numC(pc_));
        if (b.imm) e_.mov_imm(SCRATCH0, b.value); else e_.mov(SCRATCH0, b.reg);
        emit_branch_indirect(SCRATCH0, false);
        return;
      }
      add_pending(numC(pc_));
      if (b.imm) e_.mov_imm(host_reg(rd), b.value); else e_.mov(host_reg(rd), b.reg);
      return;
    default:
      add_pending(numC(pc_));
      return;
    }
  }
  case TOp::BxBlx: {
    const u32 rs = (instr >> 3) & 0xF;
    if (instr & (1 << 7)) {
      if (!a9_) { emit_fallback(instr, true, true); return; }
      if (rs == 15) e_.mov_imm(SCRATCH0, pc_ + 4); else e_.mov(SCRATCH0, host_reg(rs));
      e_.mov_imm(host_reg(14), (pc_ + 2) | 1);
      emit_branch_indirect(SCRATCH0, true);
      return;
    }
    if (rs == 15) e_.mov_imm(SCRATCH0, pc_ + 4); else e_.mov(SCRATCH0, host_reg(rs));
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::LdrPcRel: case TOp::LdrStrReg: case TOp::LdrStrImm5: case TOp::LdrStrHImm5: case TOp::LdrStrSpRel:
    thumb_ldr_str(instr, op);
    return;
  case TOp::AddPcSp: {
    const u32 rd = (instr >> 8) & 7, imm = (instr & 0xFF) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 11)) e_.add_imm(host_reg(rd), host_reg(13), imm);
    else e_.mov_imm(host_reg(rd), ((pc_ + 4) & ~3u) + imm);
    return;
  }
  case TOp::AdjustSp: {
    const u32 imm = (instr & 0x7F) * 4;
    add_pending(numC(pc_));
    if (instr & (1 << 7)) e_.sub_imm(host_reg(13), host_reg(13), imm); else e_.add_imm(host_reg(13), host_reg(13), imm);
    return;
  }
  case TOp::PushPop: {
    u32 list = instr & 0xFF;
    const bool pop = instr & (1 << 11), r = instr & (1 << 8);
    if (pop) {
      if (r) list |= 0x8000;
      const u32 n = static_cast<u32>(__builtin_popcount(list));
      if (n == 0) { emit_fallback(instr, false, false); return; }
      e_.mov(SCRATCH1, host_reg(13));
      e_.add_imm(SCRATCH7, host_reg(13), n * 4);
      // POP always writes back (r13 is never in a Thumb list).
      emit_block_transfer(instr, list, true, true, 13, a9_, 0);
      return;
    }
    if (r) list |= 0x4000;
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    if (n == 0) { emit_fallback(instr, false, false); return; }
    e_.sub_imm(SCRATCH1, host_reg(13), n * 4);
    e_.mov(SCRATCH7, SCRATCH1);
    emit_block_transfer(instr, list, false, true, 13, false, 0);
    return;
  }
  case TOp::StmLdm: {
    const u32 rb = (instr >> 8) & 7, list = instr & 0xFF;
    const bool load = instr & (1 << 11);
    if (list == 0) { emit_fallback(instr, true, load); return; }
    const u32 n = static_cast<u32>(__builtin_popcount(list));
    e_.mov(SCRATCH1, host_reg(rb));
    e_.add_imm(SCRATCH7, host_reg(rb), n * 4);
    emit_block_transfer(instr, list, load, true, rb, false, 0);
    return;
  }
  case TOp::BCond: {
    const u32 cond = (instr >> 8) & 0xF;
    const s32 off = static_cast<s8>(instr & 0xFF) * 2;
    flush_pending();
    size_t skip = e_.b_cond_fwd(invert(static_cast<Cond>(cond)));
    emit_branch_static(pc_ + 4 + static_cast<u32>(off), true, true);
    e_.bind(skip);
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
    e_.mov_imm(host_reg(14), bl_prefix_lr_);
    bl_prefix_valid_ = true;
    return;
  }
  case TOp::BlSuffix: case TOp::BlxSuffix: {
    const bool blx = op == TOp::BlxSuffix;
    if (blx && !a9_) { emit_fallback(instr, true, true); return; }
    const u32 ret = (pc_ + 2) | 1;
    if (was_prefix) {
      u32 target = bl_prefix_lr_ + ((instr & 0x7FF) << 1);
      e_.mov_imm(host_reg(14), ret);
      if (blx) emit_branch_static(target & ~3u, false, true);
      else emit_branch_static(target & ~1u, true, true);
      return;
    }
    e_.add_imm_any(SCRATCH0, host_reg(14), (instr & 0x7FF) << 1, SCRATCH2);
    e_.mov_imm(host_reg(14), ret);
    if (blx) e_.and_imm(SCRATCH0, SCRATCH0, ~3u); else e_.orr_imm(SCRATCH0, SCRATCH0, 1);
    emit_branch_indirect(SCRATCH0, true);
    return;
  }
  case TOp::Swi: emit_fallback(instr, true, true); return;
  case TOp::Bkpt: emit_fallback(instr, true, true); return;
  case TOp::Undefined: emit_fallback(instr, true, true); return;
  }
}

// ---- block driver ---------------------------------------------------------------------------------------------------

bool Translator::run() {
  const u32 start = key_pc(key_);
  const u32 step = thumb_ ? 2 : 4;
  if (!a9_) { t7_ = cpu_.timing7[start >> 15]; code_region7_ = start >> 24; }

  // Decode the straight-line run.
  u32 addr = start;
  for (u32 i = 0; i < MAX_INSTRS; ++i) {
    const u32 raw = fetch(addr);
    instrs_.push_back({addr, raw, F_ALL});
    const bool ends = thumb_ ? thumb_ends_block(static_cast<u16>(raw)) : arm_ends_block(raw);
    addr += step;
    if (ends) break;
    if (!cpu_.page_table.read_ptr(addr)) break;   // do not walk into unmapped space
  }
  blk_.guest_len = addr - start;

  // Backward flag liveness.
  u32 live = F_ALL;
  for (size_t i = instrs_.size(); i-- > 0;) {
    instrs_[i].live_out = live;
    const FlagUse u = thumb_ ? thumb_flag_use(static_cast<u16>(instrs_[i].raw), a9_) : arm_flag_use(instrs_[i].raw, a9_);
    live = u.reads | (live & ~u.writes);
  }

  // Prologue: leave when the budget is exhausted (>= 3 instructions so an
  // invalidated block can be redirected in place).
  size_t ok = 0;
  {
    e_.sub_imm(SCRATCH0, R_BUDGET, 1);
    ok = e_.tbz_fwd(SCRATCH0, 31);
    emit_exit_key(key_);
    e_.bind(ok);
  }

  for (const Instr& in : instrs_) {
    if (e_.remaining() < 4096) return false;
    pc_ = in.addr;
    live_ = in.live_out;
    if (rt().trace) { flush_pending(); emit_trace(in.raw); }
    if (rt().cyclog) { flush_pending(); emit_call_full(reinterpret_cast<const void*>(&jit_h_cyclog), in.raw, make_key(in.addr, thumb_)); }
    if (thumb_) translate_thumb(static_cast<u16>(in.raw)); else translate_arm(in.raw);
    rt().stats.instrs_translated++;
    if (ended_) break;
    if (rt().strict) {
      // The interpreter tests the budget before every instruction; reproduce
      // that so the two engines interleave identically (verification mode).
      flush_pending();
      e_.sub_imm(SCRATCH0, R_BUDGET, 1);
      size_t fine = e_.tbz_fwd(SCRATCH0, 31);
      emit_exit_key(make_key(in.addr + step, thumb_));
      e_.bind(fine);
    }
  }
  if (!ended_) emit_branch_static(addr, thumb_, false);
  return true;
}

} // namespace

bool translate_block(JitCpu& jc, u32 key, Emitter& e, Block& b) {
  Translator t(jc, key, e, b);
  return t.run();
}

} // namespace ds::jit
