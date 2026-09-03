// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) backend: the entry/exit/call/dispatch stubs, and the code
// patches the common runtime needs (entry redirect, link patch, relative
// branch test). See a32/convention.h for the register map.
//
// Stub calling convention, as on AArch64: a block reaches a stub with `bl`
// and the stub reads its literal arguments from the words after the `bl`
// through lr, skipping them before it returns with `bx lr`. A linked
// `bl link; .word key` becomes a bare `b` whose literal is never executed.
//
// Two AAPCS32 points the AArch64 stubs did not have to know:
//  - C helpers are Thumb-2 on the handheld toolchains, so they are called
//    with `blx r12`, never `bl`; they return to ARM state through lr.
//  - A struct of two pointers (SliceNext) is returned in memory: the caller
//    passes the result address in r0 and the arguments shift up one.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/jit/a32/convention.h"
#include "core/cpu/jit/a32/emit.h"
#include "core/sched/scheduler.h"

#include <cstring>
#include <vector>

namespace ds::jit {

extern "C" ds::SliceNext ds_slice_next(void* scheduler);

namespace {

constexpr u32 M(u32 r) { return 1u << r; }
constexpr u32 CALLEE_SAVED = M(4) | M(5) | M(6) | M(7) | M(8) | M(9) | M(10) | M(11);

// The budget register holds `cycle_budget - 1`; the C side sees the real value.
void emit_store_budget(Emitter& e) {
  e.add_imm(SCRATCH0, R_BUDGET, 1, SCRATCH0);
  e.str(SCRATCH0, R_CTX, OFF_BUDGET);
}
void emit_load_budget(Emitter& e) {
  e.ldr(R_BUDGET, R_CTX, OFF_BUDGET);
  e.sub_imm(R_BUDGET, R_BUDGET, 1, SCRATCH0);
}
void emit_load_pt(Emitter& e) {
  e.ldr(R_PT, R_CTX, OFF_JIT);
  e.ldr(R_PT, R_PT, OFF_JC_PT);
}

// Leave-or-continue after a helper: budget exhausted, an alert, or an IRQ
// pending with I clear. Fills `leave` with forward branches to bind at the
// leave path. Host flags are free in this backend (guest NZCV live in memory).
void emit_poll(Emitter& e, std::vector<size_t>& leave) {
  e.tst_imm(R_BUDGET, 0x80000000u);
  leave.push_back(e.b_fwd(NE));
  e.ldr(SCRATCH0, R_CTX, OFF_ALERTS);
  e.cmp_imm(SCRATCH0, 0);
  leave.push_back(e.b_fwd(NE));
  e.ldr(SCRATCH0, R_CTX, OFF_IRQ);
  e.cmp_imm(SCRATCH0, 0);
  size_t none = e.b_fwd(EQ);
  e.ldr(SCRATCH1, R_CTX, OFF_CPSR);
  e.tst_imm(SCRATCH1, 0x80);              // I
  leave.push_back(e.b_fwd(EQ));
  e.bind(none);
}

} // namespace

namespace backend {

void emit_stubs(Runtime& rt) {
  Emitter e(rt.arena, rt.cap);
  e.set_pos(LUT_AREA);

  // Stack discipline: AAPCS wants sp 8-byte aligned at every C call, and NEON
  // code below ds_slice_next stores to the stack with :64 alignment -- a
  // misaligned frame here is a SIGBUS deep in the renderer. Every frame in
  // this file is a multiple of 8 bytes, so the block level and the C calls
  // both see an aligned sp.
  // ---- enter(ctx, native): C-callable ---------------------------------------------
  rt.enter = reinterpret_cast<void (*)(CpuContext*, const void*)>(e.cur());
  e.push(CALLEE_SAVED | M(R_LR));            // 36 bytes
  e.dp_imm(SUB, false, R_SP, R_SP, 4);       // 40
  size_t to_light = e.bl_fwd();
  e.dp_imm(ADD, false, R_SP, R_SP, 4);
  e.pop(CALLEE_SAVED | M(R_PC));
  // ---- enter_light(ctx, native): an 8-byte frame; exits return here -------------------
  e.bind(to_light);
  rt.enter_light = e.cur();
  e.push(M(4) | M(R_LR));
  e.mov(R_CTX, 0);
  emit_load_budget(e);
  emit_load_pt(e);
  e.bx(1);

  // ---- exit_key_lit: `bl exit_key_lit; .word key` ---------------------------------
  rt.exit_key_lit = e.cur();
  e.ldr(0, R_LR, 0);
  // ---- exit_key: r0 = key of the next instruction ---------------------------------
  rt.exit_key = e.cur();
  e.and_imm(1, 0, 1);                       // T
  e.and_imm(2, 0, ~1u);                     // pc
  e.add_imm(2, 2, 8, SCRATCH3);
  e.sub_reg(2, 2, 1, LSL, 2);               // pc + 8 - 4T
  e.str(2, R_CTX, off_reg(15));
  // ---- exit_r15: ctx.r15 already correct -------------------------------------------
  rt.exit_r15 = e.cur();
  emit_store_budget(e);
  e.pop(M(4) | M(R_PC));

  // ---- run_loop(scheduler): the native slice loop -----------------------------------
  // SliceNext comes back in memory: r0 = &result, r1 = scheduler.
  rt.run_loop = reinterpret_cast<void (*)(void*)>(e.cur());
  e.push(CALLEE_SAVED | M(R_LR));           // 36 bytes
  e.dp_imm(SUB, false, R_SP, R_SP, 20);     // 56: [sp] = SliceNext, [sp+8] = scheduler
  e.str(0, R_SP, 8);
  const size_t loop_top = e.size();
  e.mov(0, R_SP);
  e.ldr(1, R_SP, 8);
  e.mov_ptr(R_FN, reinterpret_cast<const void*>(&ds_slice_next));
  e.blx(R_FN);
  e.ldr(0, R_SP, 0);
  e.ldr(1, R_SP, 4);
  e.cmp_imm(0, 0);
  size_t loop_exit = e.b_fwd(EQ);
  e.bl(rt.enter_light);
  e.b(e.base() + loop_top);
  e.bind(loop_exit);
  e.dp_imm(ADD, false, R_SP, R_SP, 20);
  e.pop(CALLEE_SAVED | M(R_PC));

  // ---- flush_exit: r0 = key; arena full --------------------------------------------
  rt.flush_exit = e.cur();
  e.mov_ptr(1, &rt.need_reset);
  e.mov_imm(2, 1);
  e.strb(2, 1, 0);
  e.b(rt.exit_r15);

  // ---- call_pure / call_full: r12 = fn, args in r0-r3 -------------------------------
  // Nothing guest-side lives in host registers yet, so both are the same:
  // spill the budget (helpers read and charge it), call, reload.
  rt.call_pure = e.cur();
  e.push(M(4) | M(R_LR));
  e.mov(4, R_FN);                           // r12 is not preserved across the stores below
  e.add_imm(R_LR, R_BUDGET, 1, R_LR);       // lr is free until the blx: spill through it
  e.str(R_LR, R_CTX, OFF_BUDGET);
  e.blx(4);
  emit_load_budget(e);
  e.pop(M(4) | M(R_PC));
  rt.call_full = rt.call_pure;

  // ---- call2: `bl call2; .word a; .word b; .word fn` -> fn(ctx, a, b) -----------------
  rt.call2 = e.cur();
  e.ldr(1, R_LR, 0);
  e.ldr(2, R_LR, 4);
  e.ldr(R_FN, R_LR, 8);
  e.add_imm(R_LR, R_LR, 12, SCRATCH3);
  e.push(M(4) | M(R_LR));
  e.mov(0, R_CTX);
  e.bl(rt.call_full);
  e.pop(M(4) | M(R_PC));

  // ---- poll: `bl poll; .word next_key` --------------------------------------------------
  rt.poll = e.cur();
  {
    std::vector<size_t> leave;
    emit_poll(e, leave);
    e.add_imm(R_LR, R_LR, 4, SCRATCH0);
    e.bx(R_LR);
    for (size_t f : leave) e.bind(f);
    e.ldr(0, R_LR, 0);
    e.b(rt.exit_key);
  }

  // Slow accesses and flag merges: nothing inlines memory yet, so no stub.
  for (int k = 0; k < 3; ++k) { rt.slow_load[k] = nullptr; rt.slow_store[k] = nullptr; }
  rt.merge_keep_cv = rt.merge_set_c = nullptr;

  // ---- per-CPU stubs ------------------------------------------------------------------
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = rt.cpus[c];
    const u8* lut = rt.arena + c * LUT_STRIDE;

    // dispatch: r0 = key. LUT entry = (native offset << 32) | key, 8 bytes.
    jc.dispatch = e.cur();
    e.ubfx(2, 0, 1, LUT_BITS);
    e.mov_ptr(1, lut);
    e.add_reg(1, 1, 2, LSL, 3);
    e.ldrd(2, 1, 0);                        // r2 = key, r3 = offset
    e.cmp_reg(2, 0);
    size_t miss = e.b_fwd(NE);
    e.mov_ptr(1, rt.arena);
    e.add_reg(R_PC, 1, 3);
    e.bind(miss);
    e.mov(1, 0);
    e.mov(0, R_CTX);
    e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_lookup));
    e.bl(rt.call_pure);
    e.bx(0);

    // link: `bl link; .word key`
    jc.link = e.cur();
    e.ldr(1, R_LR, 0);
    e.sub_imm(2, R_LR, 4, SCRATCH3);        // patch site
    e.mov(0, R_CTX);
    e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_link));
    e.bl(rt.call_pure);
    e.bx(0);

    // fallback: `bl fallback; .word instr; .word key`
    jc.fallback = e.cur();
    {
      e.ldr(1, R_LR, 0);
      e.ldr(2, R_LR, 4);
      e.add_imm(R_LR, R_LR, 8, SCRATCH3);
      e.push(M(2) | M(R_LR));               // key, return address
      e.mov(0, R_CTX);
      e.mov_ptr(R_FN, reinterpret_cast<const void*>(&jit_h_fallback));
      e.bl(rt.call_full);
      e.cmp_imm(0, 0);
      size_t jumped = e.b_fwd(NE);
      std::vector<size_t> leave;
      emit_poll(e, leave);
      e.pop(M(2) | M(R_PC));                // continue with the block
      for (size_t f : leave) e.bind(f);
      e.pop(M(2) | M(R_LR));
      e.and_imm(3, 2, 1);                   // next key = key + 4 - 2T
      e.add_imm(0, 2, 4, SCRATCH0);
      e.sub_reg(0, 0, 3, LSL, 1);
      e.b(rt.exit_key);
      e.bind(jumped);
      e.pop(M(2) | M(R_LR));
      std::vector<size_t> leave2;
      emit_poll(e, leave2);
      e.ldr(0, R_CTX, off_reg(15));
      e.ldr(1, R_CTX, OFF_CPSR);
      e.ubfx(2, 1, 5, 1);                   // T
      e.sub_imm(0, 0, 8, SCRATCH3);
      e.add_reg(0, 0, 2, LSL, 2);           // r15 - 8 + 4T
      e.orr_reg(0, 0, 2);                   // | T
      e.b(jc.dispatch);
      for (size_t f : leave2) e.bind(f);
      e.b(rt.exit_r15);
    }

    // Indirect branches are interpreter fallbacks in this phase.
    jc.branch_indirect = jc.branch_indirect_cdi = nullptr;
  }

  rt.stubs_end = (e.size() + 63) & ~size_t{63};
  rt.pos = rt.stubs_end;
  sync_icache(rt.arena + LUT_AREA, rt.stubs_end - LUT_AREA);
}

// Killed block: `movw r0; movt r0; b dispatch` = the 12 bytes of ENTRY_PATCH.
void write_entry_redirect(u8* entry, u32 key, const u8* dispatch) {
  Emitter e(entry, ENTRY_PATCH);
  e.movw(0, key & 0xFFFF);
  e.movt(0, key >> 16);
  e.b(dispatch);
}

// `bl link` at `site` becomes `b target` (same encoding, L bit clear).
void patch_link(u8* site, const u8* target) {
  Emitter::patch(site, 0xEA000000u | Emitter::rel24_from(site, target));
}

// b / bl of any condition: class by L bit and condition.
u32 relative_branch_class(u32 w) {
  if ((w & 0x0E000000u) == 0x0A000000u) return 1 + ((w >> 24) & 1) + ((w >> 28) << 1);
  return 0;
}

} // namespace backend
} // namespace ds::jit
