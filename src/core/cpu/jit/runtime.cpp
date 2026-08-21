// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Recompiler runtime: code arena, the hand-built entry/exit/call/dispatch
// stubs (emitted with the same encoder the translator uses, so there is no
// assembler dependency), the block cache, block linking, and self-modifying
// code tracking by host page.
//
// Stub calling convention: a block reaches a stub with `bl`, and the stub
// reads its literal arguments (instruction word, key, ...) from the words
// that follow the `bl` through x30, skipping them before it returns. A call
// site is therefore the `bl` plus its literals; nothing is materialised in
// registers at the site, and a linked `bl link; .word key` becomes a bare
// `b` whose literal is never executed.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/cpu_cycles.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/nds.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <algorithm>
#include <vector>

namespace ds::jit {

namespace {
constexpr size_t ARENA_BYTES = 64u << 20;
constexpr size_t BLOCK_MARGIN = 64u << 10;    // a block may emit up to this much

Runtime g_rt;

// ---- stub emission ---------------------------------------------------------------

void emit_store_callee_saved_guest(Emitter& e) {
  for (u32 r = 0; r < 8; r += 2) e.stp_w(host_reg(r), host_reg(r + 1), R_CTX, off_reg(r));
  e.stp_w(host_reg(13), host_reg(14), R_CTX, off_reg(13));
}
void emit_load_callee_saved_guest(Emitter& e) {
  for (u32 r = 0; r < 8; r += 2) e.ldp_w(host_reg(r), host_reg(r + 1), R_CTX, off_reg(r));
  e.ldp_w(host_reg(13), host_reg(14), R_CTX, off_reg(13));
}
// w8 holds budget - 1; the context holds the budget (x17 is free in stubs).
void emit_store_caller_saved_guest(Emitter& e) {
  e.stp_w(host_reg(8), host_reg(9), R_CTX, off_reg(8));
  e.stp_w(host_reg(10), host_reg(11), R_CTX, off_reg(10));
  e.str_w(host_reg(12), R_CTX, off_reg(12));
  e.add_imm(17, R_BUDGET, 1);
  e.str_w(17, R_CTX, OFF_BUDGET);
}
void emit_load_caller_saved_guest(Emitter& e) {
  e.ldp_w(host_reg(8), host_reg(9), R_CTX, off_reg(8));
  e.ldp_w(host_reg(10), host_reg(11), R_CTX, off_reg(10));
  e.ldr_w(host_reg(12), R_CTX, off_reg(12));
  e.ldr_w(R_BUDGET, R_CTX, OFF_BUDGET);
  e.sub_imm(R_BUDGET, R_BUDGET, 1);
  e.ldr_x(17, R_CTX, OFF_JIT);
  e.ldr_x(R_PT, 17, OFF_JC_PT);
  e.ldr_x(R_TIM, 17, OFF_JC_TIM);
}
// Merge host NZCV into ctx.cpsr (tmp registers: 17 and 30 are free in stubs).
void emit_save_flags(Emitter& e, u32 t0, u32 t1) {
  e.mrs_nzcv(t0);
  e.ldr_w(t1, R_CTX, OFF_CPSR);
  e.and_imm(t1, t1, 0x0FFFFFFF);
  e.orr_reg(t1, t1, t0);
  e.str_w(t1, R_CTX, OFF_CPSR);
}
void emit_load_flags(Emitter& e, u32 t0) {
  e.ldr_w(t0, R_CTX, OFF_CPSR);
  e.msr_nzcv(t0);
}

// Prefetch cost of one ARM9 fetch at address in `wa`, result in `wc`, using
// cmp/csel (the stubs save and restore the guest flags around this).
// c = tbl[a>>12][0]; c == 0xFF ? ((branch || !(a & 0x1F)) ? 3 : 1) : c
void emit_fetch_cost9(Emitter& e, u32 wa, u32 wc, u32 t, bool branch) {
  e.lsr_imm(t, wa, 12);
  e.add_reg(t, R_TIM, t, LSL, 2, true);
  e.ldrb(wc, t, 0);
  e.cmp_imm(wc, 0xFF);
  if (branch) {
    e.movz(t, 3);
    e.csel(wc, t, wc, EQ);
  } else {
    size_t skip = e.b_cond_fwd(NE);
    e.tst_imm(wa, 0x1F);
    e.movz(t, 3);
    e.movz(wc, 1);
    e.csel(wc, t, wc, EQ);
    e.bind(skip);
  }
}

// Poll after a helper: `leave` receives the fixups to bind at the leave
// code; execution falls through when the block continues. Clobbers w3.
void emit_poll(Emitter& e, std::vector<size_t>& leave) {
  leave.push_back(e.tbnz_fwd(R_BUDGET, 31));
  e.ldr_w(3, R_CTX, OFF_ALERTS);
  leave.push_back(e.cbnz_fwd(3));
  e.ldr_w(3, R_CTX, OFF_IRQ);
  size_t ok = e.cbz_fwd(3);
  e.ldr_w(3, R_CTX, OFF_CPSR);
  size_t ok2 = e.tbnz_fwd(3, 7);
  leave.push_back(e.b_fwd());
  e.bind(ok);
  e.bind(ok2);
}

void emit_stubs(Runtime& rt) {
  Emitter e(rt.arena, rt.cap);

  // ---- enter(ctx, native) -------------------------------------------------------
  rt.enter = reinterpret_cast<void (*)(CpuContext*, const void*)>(e.cur());
  e.stp_x_pre(29, 30, SP, -96);
  e.stp_x(19, 20, SP, 16);
  e.stp_x(21, 22, SP, 32);
  e.stp_x(23, 24, SP, 48);
  e.stp_x(25, 26, SP, 64);
  e.stp_x(27, 28, SP, 80);
  e.mov(R_CTX, 0, true);
  emit_load_callee_saved_guest(e);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.br(1);

  // ---- exit_key_lit: `bl exit_key_lit; .word key` -------------------------------
  rt.exit_key_lit = e.cur();
  e.ldr_w(0, 30, 0);
  // fall through
  // ---- exit_key: w0 = key of the next instruction -------------------------------
  rt.exit_key = e.cur();
  e.and_imm(1, 0, 1);                 // T
  e.and_imm(2, 0, ~1u);               // pc
  e.add_imm(2, 2, 8);
  e.sub_reg(2, 2, 1, LSL, 2);         // pc + 8 - 4T
  e.str_w(2, R_CTX, off_reg(15));
  // fall through
  rt.exit_r15 = e.cur();
  emit_store_callee_saved_guest(e);
  emit_store_caller_saved_guest(e);
  emit_save_flags(e, 17, 30);
  e.ldp_x(19, 20, SP, 16);
  e.ldp_x(21, 22, SP, 32);
  e.ldp_x(23, 24, SP, 48);
  e.ldp_x(25, 26, SP, 64);
  e.ldp_x(27, 28, SP, 80);
  e.ldp_x_post(29, 30, SP, 96);
  e.ret();

  // ---- flush_exit: w0 = key; arena full ----------------------------------------
  rt.flush_exit = e.cur();       // ctx.r15 already set by the helper
  e.mov_imm64(1, reinterpret_cast<u64>(&rt.need_reset));
  e.movz(2, 1);
  e.strb(2, 1, 0);
  e.b(rt.exit_r15);

  // ---- call_pure: x16 = fn, args in x0-x3 ------------------------------------------
  rt.call_pure = e.cur();
  e.str_x_pre(30, SP, -16);
  emit_save_flags(e, 17, 30);
  emit_store_caller_saved_guest(e);
  e.blr(R_FN);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- call_full: x16 = fn, args in x0-x3 ------------------------------------------
  rt.call_full = e.cur();
  e.str_x_pre(30, SP, -16);
  emit_save_flags(e, 17, 30);
  emit_store_caller_saved_guest(e);
  emit_store_callee_saved_guest(e);
  e.blr(R_FN);
  emit_load_callee_saved_guest(e);
  emit_load_caller_saved_guest(e);
  emit_load_flags(e, 17);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- call2: `bl call2; .word a; .word b; .xword fn` -> call_full fn(ctx, a, b) ----
  rt.call2 = e.cur();
  e.ldp_w(1, 2, 30, 0);
  e.ldr_x(R_FN, 30, 8);
  e.add_imm(30, 30, 16, true);
  e.str_x_pre(30, SP, -16);
  e.mov(0, R_CTX, true);
  e.bl(rt.call_full);
  e.ldr_x_post(30, SP, 16);
  e.ret();

  // ---- poll: `bl poll; .word next_key` ----------------------------------------------
  rt.poll = e.cur();
  {
    std::vector<size_t> leave;
    emit_poll(e, leave);
    e.add_imm(30, 30, 4, true);
    e.ret();
    for (size_t f : leave) e.bind(f);
    e.ldr_w(0, 30, 0);
    e.b(rt.exit_key);
  }

  // ---- slow loads/stores: w1 = address (w2 = value); x1, x7 preserved -------------
  {
    const void* lds[3] = {reinterpret_cast<const void*>(&jit_h_ld8), reinterpret_cast<const void*>(&jit_h_ld16), reinterpret_cast<const void*>(&jit_h_ld32)};
    const void* sts[3] = {reinterpret_cast<const void*>(&jit_h_st8), reinterpret_cast<const void*>(&jit_h_st16), reinterpret_cast<const void*>(&jit_h_st32)};
    for (int k = 0; k < 6; ++k) {
      (k < 3 ? rt.slow_load[k] : rt.slow_store[k - 3]) = e.cur();
      e.stp_x_pre(30, 1, SP, -32);
      e.str_x(7, SP, 16);
      e.mov(0, R_CTX, true);
      e.mov_imm64(R_FN, reinterpret_cast<u64>(k < 3 ? lds[k] : sts[k - 3]));
      e.bl(rt.call_pure);
      e.ldr_x(7, SP, 16);
      e.ldp_x_post(30, 1, SP, 32);
      e.ret();
    }
  }

  // ---- flag merges ---------------------------------------------------------------------
  // merge_keep_cv: w0 = result. N,Z from the result; C,V unchanged.
  rt.merge_keep_cv = e.cur();
  e.mrs_nzcv(1);
  e.tst_reg(0, 0);
  e.mrs_nzcv(2);
  e.ubfx(3, 1, 28, 2, true);
  e.bfi(2, 3, 28, 2, true);
  e.msr_nzcv(2);
  e.ret();
  // merge_set_c: w0 = result, w1 = carry (0/1). N,Z from the result, C from w1, V unchanged.
  rt.merge_set_c = e.cur();
  e.mrs_nzcv(2);
  e.tst_reg(0, 0);
  e.mrs_nzcv(3);
  e.ubfx(2, 2, 28, 1, true);
  e.bfi(3, 2, 28, 1, true);
  e.bfi(3, 1, 29, 1, true);
  e.msr_nzcv(3);
  e.ret();

  // ---- per-CPU stubs -----------------------------------------------------------------
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = rt.cpus[c];

    // dispatch: w0 = key
    jc.dispatch = e.cur();
    e.mov_imm64(1, reinterpret_cast<u64>(jc.lut));
    e.ubfx(2, 0, 1, LUT_BITS);
    e.ldr_x_reg(3, 1, 2, true, true);
    e.eor_reg(4, 3, 0);
    size_t miss = e.cbnz_fwd(4);
    e.lsr_imm(3, 3, 32, true);
    e.mov_imm64(5, reinterpret_cast<u64>(rt.arena));
    e.add_reg(3, 5, 3, LSL, 0, true);
    e.br(3);
    e.bind(miss);
    e.mov(1, 0);
    e.mov(0, R_CTX, true);
    e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_lookup));
    e.bl(rt.call_pure);
    e.br(0);

    // link: `bl link; .word key`
    jc.link = e.cur();
    e.ldr_w(1, 30, 0);
    e.sub_imm(2, 30, 4, true);          // patch site
    e.mov(0, R_CTX, true);
    e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_link));
    e.bl(rt.call_pure);
    e.br(0);

    // fallback: `bl fallback; .word instr; .word key`. Runs the instruction
    // through the interpreter, polls, then returns to the block when the
    // instruction did not jump, or dispatches on the new pc.
    jc.fallback = e.cur();
    {
      e.ldp_w(1, 2, 30, 0);
      e.add_imm(30, 30, 8, true);
      e.stp_x_pre(30, 2, SP, -16);
      e.mov(0, R_CTX, true);
      e.mov_imm64(R_FN, reinterpret_cast<u64>(&jit_h_fallback));
      e.bl(rt.call_full);
      size_t jumped = e.cbnz_fwd(0);
      std::vector<size_t> leave;
      emit_poll(e, leave);
      e.ldp_x_post(30, 2, SP, 16);
      e.ret();
      for (size_t f : leave) e.bind(f);
      e.ldp_x_post(30, 2, SP, 16);
      e.and_imm(3, 2, 1);               // next key = key + 4 - 2T
      e.add_imm(0, 2, 4);
      e.sub_reg(0, 0, 3, LSL, 1);
      e.b(rt.exit_key);
      e.bind(jumped);
      e.ldp_x_post(30, 2, SP, 16);
      std::vector<size_t> leave2;
      emit_poll(e, leave2);
      // dispatch from the context: key = (r15 - 8 + 4T) | T
      e.ldr_w(0, R_CTX, off_reg(15));
      e.ldr_w(1, R_CTX, OFF_CPSR);
      e.ubfx(2, 1, 5, 1);
      e.sub_imm(0, 0, 8);
      e.add_reg(0, 0, 2, LSL, 2);
      e.orr_reg(0, 0, 2);
      e.b(jc.dispatch);
      for (size_t f : leave2) e.bind(f);
      e.b(rt.exit_r15);
    }

    // branch_indirect_cdi: w0 = target (bit 0 = new T), w1 = numD, w2 = data
    // address. Charges the CDI cost of an LDM/POP that loaded pc the way the
    // interpreter does *after* the jump (new pc, state and code region), then
    // continues as branch_indirect. Flags are saved in x17 throughout.
    jc.branch_indirect_cdi = e.cur();
    e.mrs_nzcv(17);
    {
      size_t is_thumb = e.tbnz_fwd(0, 0);
      e.and_imm(0, 0, ~3u);
      e.bind(is_thumb);
      e.and_imm(3, 0, ~1u);                 // a
      if (c == 0) {
        // numC = (T && (a + 4) & 2) ? 0 : code_after;  code_after: ARM cost(a+4,S); Thumb a&2 ? cost(a+2,S) : cost(a,B)
        size_t thumb = e.tbnz_fwd(0, 0);
        e.add_imm(5, 3, 4);
        emit_fetch_cost9(e, 5, 4, 6, false);
        size_t done = e.b_fwd();
        e.bind(thumb);
        size_t odd = e.tbnz_fwd(3, 1);
        emit_fetch_cost9(e, 3, 4, 6, true);
        e.movz(4, 0);                       // even a: new r15 = a + 4 has bit 1 set -> numC 0
        size_t done2 = e.b_fwd();
        e.bind(odd);
        e.add_imm(5, 3, 2);
        emit_fetch_cost9(e, 5, 4, 6, false);
        e.bind(done);
        e.bind(done2);
        // cost = max(numC + numD - 6, max(numC, numD))
        e.add_imm(5, 4, 0);
        e.add_reg(5, 4, 1);
        e.sub_imm(5, 5, 6);
        e.cmp_reg(4, 1);
        e.csel(6, 4, 1, HI);                // max(numC, numD)
        e.cmp_reg(5, 6);
        e.csel(5, 5, 6, GT);
      } else {
        // numC = t_new[T ? 0 : 2]; main-RAM rules with code region = target, data region = w2
        e.lsr_imm(4, 3, 15);
        e.add_reg(4, R_TIM, 4, LSL, 2, true);
        size_t thumb = e.tbnz_fwd(0, 0);
        e.ldrb(4, 4, 2);
        size_t done = e.b_fwd();
        e.bind(thumb);
        e.ldrb(4, 4, 0);
        e.bind(done);
        e.lsr_imm(5, 3, 24); e.cmp_imm(5, 2); e.cset(5, EQ);     // code_main
        e.lsr_imm(6, 2, 24); e.cmp_imm(6, 2); e.cset(6, EQ);     // data_main
        // data_main ? (code_main ? nC + d : (nC+1, max(nC+d-3, max(nC,d)))) : (code_main ? (d+1, max(...)) : nC + d + 1)
        size_t not_main = e.cbz_fwd(6);
        size_t both = e.cbnz_fwd(5);
        e.add_imm(4, 4, 1);
        size_t mx = e.b_fwd();
        e.bind(both);
        e.add_reg(5, 4, 1);
        size_t fin = e.b_fwd();
        e.bind(not_main);
        size_t plain = e.cbz_fwd(5);
        e.add_imm(1, 1, 1);
        e.bind(mx);
        // w5 = max(nC + d - 3, max(nC, d))
        e.add_reg(5, 4, 1); e.sub_imm(5, 5, 3);
        e.cmp_reg(4, 1); e.csel(6, 4, 1, HI);
        e.cmp_reg(5, 6); e.csel(5, 5, 6, GT);
        size_t fin2 = e.b_fwd();
        e.bind(plain);
        e.add_reg(5, 4, 1); e.add_imm(5, 5, 1);
        e.bind(fin);
        e.bind(fin2);
      }
      e.sub_reg(R_BUDGET, R_BUDGET, 5);
    }
    // fall into branch_indirect's body (flags already in x17)
    size_t to_body = e.b_fwd();

    // branch_indirect: w0 = target address, bit 0 = new T
    jc.branch_indirect = e.cur();
    e.mrs_nzcv(17);
    e.bind(to_body);
    size_t is_thumb = e.tbnz_fwd(0, 0);
    e.and_imm(0, 0, ~3u);
    e.bind(is_thumb);                       // Thumb keys keep bit 0; pc = key & ~1
    e.ldr_w(1, R_CTX, OFF_CPSR);
    e.bfi(1, 0, 5, 1);
    e.str_w(1, R_CTX, OFF_CPSR);
    e.and_imm(2, 0, ~1u);                   // a
    if (c == 0) {
      // ARM9 refill: ARM a: cost(a,B)+cost(a+4,S); Thumb: a&2 ? cost(a-2,B)+cost(a+2,S) : cost(a,B)
      size_t thumb = e.tbnz_fwd(0, 0);
      emit_fetch_cost9(e, 2, 3, 4, true);
      e.add_imm(5, 2, 4);
      emit_fetch_cost9(e, 5, 6, 4, false);
      e.add_reg(3, 3, 6);
      size_t done = e.b_fwd();
      e.bind(thumb);
      size_t odd = e.tbnz_fwd(2, 1);
      emit_fetch_cost9(e, 2, 3, 4, true);
      size_t done2 = e.b_fwd();
      e.bind(odd);
      e.sub_imm(5, 2, 2);
      emit_fetch_cost9(e, 5, 3, 4, true);
      e.add_imm(5, 2, 2);
      emit_fetch_cost9(e, 5, 6, 4, false);
      e.add_reg(3, 3, 6);
      e.bind(done);
      e.bind(done2);
    } else {
      // ARM7 refill: t = timing7[a >> 15]; Thumb: t0 + t1; ARM: t2 + t3
      e.lsr_imm(4, 2, 15);
      e.add_reg(4, R_TIM, 4, LSL, 2, true);
      size_t thumb = e.tbnz_fwd(0, 0);
      e.ldrb(3, 4, 2);
      e.ldrb(5, 4, 3);
      size_t done = e.b_fwd();
      e.bind(thumb);
      e.ldrb(3, 4, 0);
      e.ldrb(5, 4, 1);
      e.bind(done);
      e.add_reg(3, 3, 5);
    }
    e.sub_reg(R_BUDGET, R_BUDGET, 3);
    e.msr_nzcv(17);
    e.b(jc.dispatch);
  }

  rt.stubs_end = (e.size() + 63) & ~size_t{63};
  rt.pos = rt.stubs_end;
  sync_icache(rt.arena, rt.stubs_end);
}

// ---- code page tracking ----------------------------------------------------------------

const u8* host_page_of(const u8* p) { return reinterpret_cast<const u8*>(reinterpret_cast<u64>(p) & ~u64{mem::PAGE_SIZE - 1}); }

bool code_query(const u8* host_page) { return g_rt.code_pages.count(host_page) != 0; }

void set_code_tag(const u8* host_page, bool on) {
  for (JitCpu& jc : g_rt.cpus)
    if (jc.ctx) jc.ctx->page_table.set_code_host(host_page, on);
}

void code_write_hook(u8* host, u32 len) {
  const u8* first = host_page_of(host);
  const u8* last = host_page_of(host + len - 1);
  invalidate_host_page(first);
  if (last != first) invalidate_host_page(last);
}

void kill_block(JitCpu& jc, Block* b) {
  if (b->dead) return;
  b->dead = true;
  // Redirect the entry: anything linked to it lands in the dispatcher, which
  // misses (the LUT/map entries go below) and retranslates.
  Emitter e(b->entry, 12);
  e.movz(0, b->key & 0xFFFF);
  e.movk(0, b->key >> 16, 16);
  e.b(jc.dispatch);
  sync_icache(b->entry, 12);
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  if (static_cast<u32>(jc.lut[idx]) == b->key) jc.lut[idx] = LUT_EMPTY_KEY;
  jc.blocks.erase(b->key);
  g_rt.stats.blocks_invalidated++;
}

void remove_from_page_lists(Block* b) {
  for (u32 i = 0; i < b->npages; ++i) {
    auto it = g_rt.code_pages.find(b->host_pages[i]);
    if (it == g_rt.code_pages.end()) continue;
    auto& v = it->second;
    for (size_t k = 0; k < v.size(); ++k) if (v[k] == b) { v[k] = v.back(); v.pop_back(); break; }
    if (v.empty()) { g_rt.code_pages.erase(it); set_code_tag(b->host_pages[i], false); }
  }
}

JitCpu& jc_of(Block* b) { return g_rt.cpus[b->owner]; }

void reset_arena() {
  Runtime& r = g_rt;
  for (JitCpu& jc : r.cpus) {
    for (Block* b : jc.all_blocks) delete b;
    jc.all_blocks.clear();
    jc.blocks.clear();
    if (jc.lut) for (u32 i = 0; i < LUT_SIZE; ++i) jc.lut[i] = LUT_EMPTY_KEY;
  }
  for (auto& kv : r.code_pages) set_code_tag(kv.first, false);
  r.code_pages.clear();
  r.pos = r.stubs_end;
  r.need_reset = false;
  r.stats.flushes++;
}

void on_timing_changed(CpuContext& cpu) {
  if (cpu.jit) invalidate_cpu(*static_cast<JitCpu*>(cpu.jit));
}

} // namespace

Runtime& rt() { return g_rt; }

// ---- cache -----------------------------------------------------------------------------

void lut_insert(JitCpu& jc, Block* b) {
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  jc.lut[idx] = (static_cast<u64>(b->entry - g_rt.arena) << 32) | b->key;
}

Block* translate(JitCpu& jc, u32 key) {
  Runtime& r = g_rt;
  if (r.pos + BLOCK_MARGIN > r.cap) return nullptr;
  Block* b = new Block{};
  b->key = key;
  b->owner = jc.arm9 ? 0 : 1;
  Emitter e(r.arena + r.pos, BLOCK_MARGIN);
  if (!translate_block(jc, key, e, *b)) { delete b; return nullptr; }
  b->entry = r.arena + r.pos;
  b->size = static_cast<u32>(e.size());
  if (r.debug) {   // DS_JIT_DEBUG: dump the block for `objdump -D -b binary -m aarch64`
    std::fprintf(stderr, "[jit] block %08x (%u bytes):", key, b->size);
    for (u32 i = 0; i < b->size; i += 4) { u32 w; std::memcpy(&w, b->entry + i, 4); std::fprintf(stderr, " %08x", w); }
    std::fputc('\n', stderr);
  }
  r.pos += (b->size + 15) & ~size_t{15};
  sync_icache(b->entry, b->size);

  // Register the host pages the guest code lives in.
  const u32 pc = key_pc(key);
  const u8* p0 = jc.ctx->page_table.read_ptr(pc);
  const u8* p1 = jc.ctx->page_table.read_ptr(pc + b->guest_len - 1);
  b->npages = 0;
  if (p0) b->host_pages[b->npages++] = host_page_of(p0);
  if (p1 && host_page_of(p1) != (p0 ? host_page_of(p0) : nullptr)) b->host_pages[b->npages++] = host_page_of(p1);
  for (u32 i = 0; i < b->npages; ++i) {
    auto& v = r.code_pages[b->host_pages[i]];
    if (v.empty()) set_code_tag(b->host_pages[i], true);
    v.push_back(b);
  }
  jc.blocks[key] = b;
  jc.all_blocks.push_back(b);
  lut_insert(jc, b);
  r.stats.blocks_translated++;
  r.stats.code_bytes += b->size;
  r.stats.hot_bytes += b->hot_size;
  return b;
}

const u8* find_native(JitCpu& jc, u32 key) {
  auto it = jc.blocks.find(key);
  if (it != jc.blocks.end()) { lut_insert(jc, it->second); return it->second->entry; }
  Block* b = translate(jc, key);
  return b ? b->entry : nullptr;
}

void invalidate_host_page(const u8* host_page) {
  auto it = g_rt.code_pages.find(host_page);
  if (it == g_rt.code_pages.end()) return;
  std::vector<Block*> victims = std::move(it->second);
  g_rt.code_pages.erase(it);
  set_code_tag(host_page, false);
  for (Block* b : victims) {
    JitCpu& jc = jc_of(b);
    kill_block(jc, b);
    // The block may also be listed under its second page.
    for (u32 i = 0; i < b->npages; ++i) if (b->host_pages[i] != host_page) {
      auto jt = g_rt.code_pages.find(b->host_pages[i]);
      if (jt == g_rt.code_pages.end()) continue;
      auto& v = jt->second;
      for (size_t k = 0; k < v.size(); ++k) if (v[k] == b) { v[k] = v.back(); v.pop_back(); break; }
      if (v.empty()) { g_rt.code_pages.erase(jt); set_code_tag(b->host_pages[i], false); }
    }
  }
  for (JitCpu& jc : g_rt.cpus) if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

void invalidate_cpu(JitCpu& jc) {
  for (Block* b : jc.all_blocks) if (!b->dead) { remove_from_page_lists(b); kill_block(jc, b); }
  jc.blocks.clear();
  if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

// ---- helpers called from translated code -----------------------------------------------------

extern "C" u32 jit_h_fallback(CpuContext* cpu, u32 instr, u32 key) {
  // Execute one instruction through the interpreter with the interpreter's own
  // cycle accounting. r15 is set from the key (pipeline-adjusted).
  cpu->hot.regs[15] = key_r15(key);
  cpu->data_cycles = 0;
  cpu->jumped = false;
  if (cpu->which == Cpu::ARM9) prefetch_cost9(*cpu);
  else { cpu->code_cycles = key_pc(key) >> 15; cpu->code_region = key_pc(key) >> 24; }
  if (key_thumb(key)) {
    interp::exec_thumb(*cpu, static_cast<u16>(instr));
    if (!cpu->jumped) cpu->hot.regs[15] += 2;
  } else {
    interp::exec_arm(*cpu, instr);
    if (!cpu->jumped) cpu->hot.regs[15] += 4;
  }
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;   // the poll leaves; run() ends the slice
  g_rt.stats.instrs_fallback++;
  if (g_rt.hist) g_rt.fallback_hist[(static_cast<u64>(cpu->which) << 32) | key_pc(key)]++;
  if (g_rt.debug) std::fprintf(stderr, "[jit] fallback %08x %08x -> r15 %08x cpsr %08x budget %d halted %d jumped %d\n", key_pc(key), instr, cpu->hot.regs[15], cpu->hot.cpsr, cpu->hot.cycle_budget, cpu->halted, cpu->jumped);
  return cpu->jumped ? 1 : 0;
}

extern "C" const void* jit_h_lookup(CpuContext* cpu, u32 key) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] lookup %08x -> %p (budget %d)\n", key, static_cast<const void*>(native), cpu->hot.cycle_budget);
  if (native) return native;
  cpu->hot.regs[15] = key_r15(key);
  return g_rt.flush_exit;   // arena full: leave; run() resets the arena
}

extern "C" const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] link %08x -> %p at %p (budget %d)\n", key, static_cast<const void*>(native), static_cast<void*>(patch_site), cpu->hot.cycle_budget);
  if (!native) { cpu->hot.regs[15] = key_r15(key); return g_rt.flush_exit; }
  // Replace `bl link` with `b native`.
  const s64 delta = native - patch_site;
  const u32 w = 0x14000000u | (static_cast<u32>(delta >> 2) & 0x03FFFFFFu);
  Emitter::patch(patch_site, w);
  sync_icache(patch_site, 4);
  return native;
}

extern "C" void jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key) {
  (void)instr;
  std::fprintf(stderr, "[cyc%d] %08x %d\n", cpu->which == Cpu::ARM9 ? 9 : 7, key_r15(key), cpu->hot.cycle_budget);
}

extern "C" void jit_h_trace(CpuContext* cpu, u32 instr, u32 key) {
  cpu->hot.regs[15] = key_r15(key);
  if (cpu->nds->trace) cpu->nds->trace(*cpu, instr, cpu->nds->trace_user);
}

// Loads and stores that left the inline page-table path: MMIO, unmapped
// space, read-only and code pages. The same paths the interpreter's
// mem_read*/mem_write* take (cpu_mem.h), minus the cost, which the block
// charges from the timing table like every other access.
extern "C" u32 jit_h_ld8(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  if (u8* p = cpu->page_table.read_ptr(addr)) return *p;
  return cpu->nds->bus.read8(cpu->which, addr);
}
extern "C" u32 jit_h_ld16(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return cpu->nds->bus.read16(cpu->which, addr);
}
extern "C" u32 jit_h_ld32(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return cpu->nds->bus.read32(cpu->which, addr);
}
extern "C" void jit_h_st8(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { *p = static_cast<u8>(v); if (code) mem::code_written(p, 1); }
  else cpu->nds->bus.write8(cpu->which, addr, static_cast<u8>(v));
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st16(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  bool code = false;
  const u16 h = static_cast<u16>(v);
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { std::memcpy(p, &h, 2); if (code) mem::code_written(p, 2); }
  else cpu->nds->bus.write16(cpu->which, addr, h);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st32(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 4); if (code) mem::code_written(p, 4); }
  else cpu->nds->bus.write32(cpu->which, addr, v);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}

// ---- public API ---------------------------------------------------------------------------------

bool attach(NDS& nds, bool arm9, bool arm7) {
  Runtime& r = g_rt;
  if (!r.arena) {
    void* p = mmap(nullptr, ARENA_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "jit: cannot map code arena\n"); return false; }
    r.arena = static_cast<u8*>(p);
    r.cap = ARENA_BYTES;
    for (int c = 0; c < 2; ++c) {
      r.cpus[c].lut = new u64[LUT_SIZE];
      for (u32 i = 0; i < LUT_SIZE; ++i) r.cpus[c].lut[i] = LUT_EMPTY_KEY;
    }
    emit_stubs(r);
    r.strict = std::getenv("DS_JIT_STRICT") != nullptr;
    r.debug = std::getenv("DS_JIT_DEBUG") != nullptr;
    r.cyclog = std::getenv("DS_DEBUG_CYCLES") != nullptr;
    r.hist = std::getenv("DS_JIT_HIST") != nullptr;
    r.fastcost = std::getenv("DS_JIT_FASTCOST") != nullptr;
    mem::PageTable::code_query = &code_query;
    mem::code_write_hook = &code_write_hook;
  }
  r.trace = nds.trace != nullptr;
  for (int c = 0; c < 2; ++c) {
    const bool on = c == 0 ? arm9 : arm7;
    if (!on) continue;
    JitCpu& jc = r.cpus[c];
    CpuContext& ctx = nds.cpu(c == 0 ? Cpu::ARM9 : Cpu::ARM7);
    jc.ctx = &ctx;
    jc.nds = &nds;
    jc.arm9 = c == 0;
    jc.hot.pt = ctx.page_table.raw();
    jc.hot.timing = reinterpret_cast<const u8*>(c == 0 ? ctx.timing9 : ctx.timing7);
    ctx.jit = &jc;
    ctx.jit_timing_changed = &on_timing_changed;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &run;
  }
  return true;
}

void detach(NDS& nds) {
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = g_rt.cpus[c];
    if (!jc.ctx) continue;
    invalidate_cpu(jc);
    jc.ctx->jit = nullptr;
    jc.ctx->jit_timing_changed = nullptr;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &interp::run;
    jc.ctx = nullptr;
  }
}

void flush(CpuContext& cpu) { if (cpu.jit) invalidate_cpu(*static_cast<JitCpu*>(cpu.jit)); }
void flush_all() { reset_arena(); }
void set_trace(bool on) { if (g_rt.trace != on) { g_rt.trace = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
const Stats& stats() { return g_rt.stats; }

void report(std::FILE* out) {
  const Stats& s = g_rt.stats;
  std::fprintf(out, "[jit] blocks %llu, inline instrs %llu, fallback executions %llu, slow accesses %llu, entries %llu, invalidated %llu, flushes %llu\n",
               (unsigned long long)s.blocks_translated, (unsigned long long)s.instrs_translated, (unsigned long long)s.instrs_fallback,
               (unsigned long long)s.slow_accesses, (unsigned long long)s.entries, (unsigned long long)s.blocks_invalidated, (unsigned long long)s.flushes);
  std::fprintf(out, "[jit] code %llu KB (hot %llu KB): %.1f bytes per guest instruction, %.1f hot\n", (unsigned long long)(s.code_bytes >> 10), (unsigned long long)(s.hot_bytes >> 10),
               s.instrs_translated ? static_cast<double>(s.code_bytes) / static_cast<double>(s.instrs_translated) : 0.0,
               s.instrs_translated ? static_cast<double>(s.hot_bytes) / static_cast<double>(s.instrs_translated) : 0.0);
  if (!g_rt.hist) return;
  std::vector<std::pair<u64, u64>> v(g_rt.fallback_hist.begin(), g_rt.fallback_hist.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::fprintf(out, "[jit] hottest fallbacks (cpu pc instr count):\n");
  for (size_t i = 0; i < v.size() && i < 16; ++i) {
    const Cpu c = static_cast<Cpu>(v[i].first >> 32);
    const u32 pc = static_cast<u32>(v[i].first);
    CpuContext& ctx = *g_rt.cpus[c == Cpu::ARM9 ? 0 : 1].ctx;
    u32 instr = 0;
    if (u8* p = ctx.page_table.read_ptr(pc)) std::memcpy(&instr, p, 4);
    std::fprintf(out, "[jit]   arm%d %08x %08x %llu\n", c == Cpu::ARM9 ? 9 : 7, pc, instr, (unsigned long long)v[i].second);
  }
}

void run(CpuContext& cpu) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu.jit);
  Runtime& r = g_rt;
  cpu.check_irq();
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  if (cpu.step_limit) { interp::run(cpu); return; }
  while (cpu.hot.cycle_budget > 0) {
    if (r.need_reset) reset_arena();
    const bool thumb = cpu.thumb();
    const u32 key = make_key(cpu.hot.regs[15] - (thumb ? 4 : 8), thumb);
    const u64 lut = jc.lut[(key >> 1) & (LUT_SIZE - 1)];
    const u8* native = static_cast<u32>(lut) == key ? r.arena + (lut >> 32) : find_native(jc, key);
    if (!native) { reset_arena(); continue; }
    cpu.hot.alerts = 0;
    r.stats.entries++;
    r.enter(&cpu, native);
    if (cpu.halted) { cpu.budget_at_halt = cpu.hot.cycle_budget; cpu.hot.cycle_budget = -1; return; }
    if (cpu.hot.irq_pending) cpu.check_irq();
  }
}

} // namespace ds::jit
