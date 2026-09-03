// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARMv7 (A32) block translator, phase 1: every guest instruction runs through
// the fallback stub, which is the interpreter with its own cycle accounting.
// What the block itself contributes is the shape the AArch64 backend has --
// the same block boundaries (block_shape.h), the budget test at entry, the
// static link at the end and, in strict mode, a budget test per instruction
// -- so the two backends interleave slices identically and the AArch64
// backend's frame hashes are this one's oracle. Inlining comes next, one
// form at a time, each checked against the interpreter by tests/jit_test.cpp.
#include "core/cpu/jit/jit_internal.h"
#include "core/cpu/jit/a32/convention.h"
#include "core/cpu/jit/a32/emit.h"
#include "core/cpu/jit/block_shape.h"
#include "core/nds.h"

#include <cstring>
#include <vector>

namespace ds::jit {

namespace {

constexpr u32 MAX_INSTRS = 64;
constexpr size_t BLOCK_LIMIT = 28u << 10;

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

  u32 step() const { return thumb_ ? 2 : 4; }

  u32 fetch(u32 addr) {
    if (u8* p = cpu_.page_table.read_ptr(addr)) {
      if (thumb_) { u16 v; std::memcpy(&v, p, 2); return v; }
      u32 v; std::memcpy(&v, p, 4); return v;
    }
    return thumb_ ? cpu_.nds->bus.read16(cpu_.which, addr) : cpu_.nds->bus.read32(cpu_.which, addr);
  }

  // Leave with `exit_key` when the budget is exhausted. No cold section yet:
  // the exit is skipped over inline (four words).
  void emit_budget_check(u32 exit_key) {
    e_.tst_imm(R_BUDGET, 0x80000000u);
    size_t ok = e_.b_fwd(EQ);
    e_.bl(rt().exit_key_lit);
    e_.word(exit_key);
    e_.bind(ok);
  }
  void emit_fallback(u32 instr, u32 pc) {
    e_.bl(jc_.fallback);
    e_.word(instr);
    e_.word(make_key(pc, thumb_));
  }
  void emit_call2(const void* fn, u32 instr, u32 key) {
    e_.bl(rt().call2);
    e_.word(instr);
    e_.word(key);
    e_.word(static_cast<u32>(reinterpret_cast<uintptr_t>(fn)));
  }
  void emit_branch_static(u32 target) {
    e_.bl(jc_.link);
    e_.word(make_key(target, thumb_));
    if (blk_.nsucc < 4) blk_.succ[blk_.nsucc++] = make_key(target, thumb_);
  }
};

bool Translator::run() {
  const u32 start = key_pc(key_);
  struct Instr { u32 addr; u32 raw; };
  std::vector<Instr> instrs;
  u32 addr = start;
  for (u32 i = 0; i < MAX_INSTRS; ++i) {
    const u32 raw = fetch(addr);
    instrs.push_back({addr, raw});
    const bool ends = thumb_ ? shape::thumb_ends_block(static_cast<u16>(raw)) : shape::arm_ends_block(raw, a9_);
    addr += step();
    if (ends) break;
    if (!cpu_.page_table.read_ptr(addr)) break;
  }

  // Nothing is baked from the timing tables: every cost is the interpreter's
  // at run time, so a retime never invalidates this translation.
  blk_.ndep = 0;
  blk_.dep_overflow = false;

  emit_budget_check(key_);
  u32 end_addr = addr;
  for (size_t i = 0; i < instrs.size(); ++i) {
    const Instr& in = instrs[i];
    if (i > 0 && e_.size() > BLOCK_LIMIT) { end_addr = in.addr; break; }
    if (e_.remaining() < 4096) return false;
    if (rt().trace) emit_call2(reinterpret_cast<const void*>(&jit_h_trace), in.raw, make_key(in.addr, thumb_));
    if (rt().cyclog) emit_call2(reinterpret_cast<const void*>(&jit_h_cyclog), in.raw, make_key(in.addr, thumb_));
    emit_fallback(in.raw, in.addr);
    if (rt().strict) emit_budget_check(make_key(in.addr + step(), thumb_));   // lockstep with the interpreter, as the A64 backend
  }
  blk_.guest_len = end_addr - start;
  // Reached only when the last instruction did not jump (the fallback stub
  // dispatches when it did): continue at the next key.
  emit_branch_static(end_addr);
  blk_.hot_size = static_cast<u32>(e_.size());
  return e_.size() >= backend::ENTRY_PATCH;
}

} // namespace

bool backend::translate_block(JitCpu& jc, u32 key, u8* buf, size_t cap, Block& b, u32& size) {
  Emitter e(buf, cap);
  Translator t(jc, key, e, b);
  if (!t.run()) return false;
  size = static_cast<u32>(e.size());
  return true;
}

} // namespace ds::jit
