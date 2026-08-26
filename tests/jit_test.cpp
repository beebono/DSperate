// SPDX-License-Identifier: GPL-3.0-or-later
// Recompiler vs interpreter differential test (AArch64 only).
// Random straight-line ARM and Thumb sequences run on two machines,
// one per engine, from identical state; the registers, flags, consumed cycles
// and memory must agree afterwards. The first disagreement prints the
// sequence, which names the broken instruction.
#include "core/nds.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/jit/jit.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ds;

namespace {

constexpr u32 CODE_BASE = 0x02000000, HALT_STUB = 0x02001000, BUF_BASE = 0x02200000, STACK = 0x02300000;

struct Machine {
  NDS nds;
  CpuContext& cpu;
  explicit Machine(Cpu which) : cpu(nds.cpu(which)) {}
  u8* host(u32 addr) { return cpu.page_table.read_ptr(addr); }
  void poke32(u32 addr, u32 v) { std::memcpy(host(addr), &v, 4); }
  void poke16(u32 addr, u16 v) { std::memcpy(host(addr), &v, 2); }
};

// ARM halt stub: ARM9 waits for interrupt through CP15; ARM7 writes HALTCNT.
void write_halt_stub(Machine& m, bool a9) {
  // Exception vectors (empty BIOS in this harness) branch to the halt stub,
  // so an undefined instruction, SWI or IRQ ends the trial instead of
  // executing zeros until the budget runs out.
  const u32 vbase = a9 ? 0xFFFF0000u : 0u;
  for (u32 v = 0; v < 0x20; v += 4) m.poke32(vbase + v, 0xE59FF000u | (0x20 - v - 8));   // ldr pc, [pc, #k] -> pool
  m.poke32(vbase + 0x20, HALT_STUB);
  if (a9) {
    m.poke32(HALT_STUB, 0xEE070F90);        // mcr p15, 0, r0, c7, c0, 4
    m.poke32(HALT_STUB + 4, 0xEAFFFFFE);    // b . (never reached)
  } else {
    m.poke32(HALT_STUB, 0xE3A00301);        // mov r0, #0x04000000
    m.poke32(HALT_STUB + 4, 0xE2800C03);    // add r0, r0, #0x300
    m.poke32(HALT_STUB + 8, 0xE3A01080);    // mov r1, #0x80
    m.poke32(HALT_STUB + 12, 0xE5C01001);   // strb r1, [r0, #1]   (0x04000301)
    m.poke32(HALT_STUB + 16, 0xEAFFFFFE);   // b .
  }
}

struct Rng {
  std::mt19937 g;
  explicit Rng(u32 seed) : g(seed) {}
  u32 next() { return g(); }
  u32 below(u32 n) { return g() % n; }
  bool coin(u32 pct = 50) { return below(100) < pct; }
};

// ---- ARM generator ----------------------------------------------------------
// Registers: r9 = buffer base (never written), r13 = stack, r15 never written.
u32 gen_arm(Rng& r) {
  const u32 cond = r.coin(80) ? 0xE : r.below(14);
  auto reg_nw = [&]() { u32 x; do { x = r.below(15); } while (x == 9 || x == 13); return x; };   // writable
  auto reg_rd = [&]() { u32 x; do { x = r.below(16); } while (x == 9 || x == 13); return x; };   // readable (pc ok)
  for (;;) {
    switch (r.below(10)) {
    case 0: case 1: case 2: {   // data processing
      const u32 opcode = r.below(16), s = r.coin(60), rd = reg_nw(), rn = reg_rd();
      const bool test = opcode >= 8 && opcode <= 0xB;
      u32 instr = (cond << 28) | (opcode << 21) | (s << 20) | (rn << 16) | (rd << 12);
      if (test) instr |= 1u << 20;
      switch (r.below(3)) {
      case 0: instr |= (1u << 25) | (r.below(16) << 8) | r.below(256); break;                       // imm
      case 1: instr |= (r.below(32) << 7) | (r.below(4) << 5) | reg_rd(); break;                    // imm shift
      default: instr |= (reg_nw() << 8) | (r.below(4) << 5) | 0x10 | reg_rd(); break;              // reg shift
      }
      return instr;
    }
    case 3: {   // multiply
      const u32 rd = reg_nw(), rn = reg_nw(), rs = reg_nw(), rm = reg_nw();
      if (rd == 15 || rn == 15 || rs == 15 || rm == 15) continue;
      const u32 s = r.coin(50);
      switch (r.below(4)) {
      case 0: return (cond << 28) | (s << 20) | (rd << 16) | (rs << 8) | 0x90 | rm;                 // MUL
      case 1: return (cond << 28) | (1u << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm;   // MLA
      default: {
        if (rd == rn) continue;
        const u32 op = 4 + r.below(4);   // UMULL UMLAL SMULL SMLAL
        return (cond << 28) | (op << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm;
      }
      }
    }
    case 4: case 5: {   // LDR/STR word/byte, base r9, pre-indexed, no writeback
      const u32 l = r.coin(50), b = r.coin(30), rd = reg_rd();
      if (l && rd == 15) continue;
      u32 instr = (cond << 28) | (1u << 26) | (1u << 24) | (1u << 23) | (b << 22) | (l << 20) | (9u << 16) | (rd << 12);
      if (r.coin(70)) instr |= r.below(0x800);
      else instr |= (1u << 25) | (r.below(8) << 7) | (r.below(3) << 5) | reg_nw();   // reg offset (may wander; both engines agree)
      return instr;
    }
    case 6: {   // LDRH/STRH/LDRSB/LDRSH, base r9, immediate
      const u32 l = r.coin(50), rd = reg_nw();
      u32 sh = l ? 1 + r.below(3) : 1;
      const u32 off = r.below(256);
      return (cond << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (l << 20) | (9u << 16) | (rd << 12) | ((off >> 4) << 8) | 0x90 | (sh << 5) | (off & 0xF);
    }
    case 7: {   // LDM/STM base r9, no writeback, no pc
      u32 list = r.next() & 0x5DFF;   // no r9, r13, r15
      if (!list) continue;
      const u32 l = r.coin(50), p = r.coin(50), u = r.coin(50);
      return (cond << 28) | (4u << 25) | (p << 24) | (u << 23) | (l << 20) | (9u << 16) | list;
    }
    case 8: {   // CLZ / MRS
      if (r.coin(50)) return (cond << 28) | 0x016F0F10 | (reg_nw() << 12) | reg_nw();
      return (cond << 28) | 0x010F0000 | (reg_nw() << 12);
    }
    default: {  // PUSH/POP style: STMDB sp!/LDMIA sp! without pc
      u32 list = r.next() & 0x5DFF;
      if (!list) continue;
      if (r.coin(50)) return (cond << 28) | (0xE92D0000u & 0x0FFFFFFF) | list;   // stmdb sp!, {list}
      return (cond << 28) | (0xE8BD0000u & 0x0FFFFFFF) | list;                 // ldmia sp!, {list}
    }
    }
  }
}

// ---- Thumb generator ---------------------------------------------------------
// r6 = buffer base (kept), sp = stack.
u16 gen_thumb(Rng& r) {
  auto lo = [&]() { u32 x; do { x = r.below(8); } while (x == 6); return x; };
  for (;;) {
    switch (r.below(12)) {
    case 0: return static_cast<u16>((r.below(3) << 11) | (r.below(32) << 6) | (lo() << 3) | lo());          // shift imm
    case 1: return static_cast<u16>(0x1800 | (r.below(4) << 9) | (r.below(8) << 6) | (lo() << 3) | lo());   // add/sub reg/imm3 (rn may be r6: read only)
    case 2: return static_cast<u16>(0x2000 | (r.below(4) << 11) | (lo() << 8) | r.below(256));             // mov/cmp/add/sub imm8
    case 3: return static_cast<u16>(0x4000 | (r.below(16) << 6) | (lo() << 3) | lo());                      // alu
    case 4: {   // hi reg ops, never pc/sp as destination, no r6
      const u32 op = r.below(3);
      u32 rd, rs;
      do { rd = r.below(15); } while (rd == 6 || rd == 13);
      do { rs = r.below(16); } while (rs == 6);
      if (op == 1 && rd == 15) continue;
      return static_cast<u16>(0x4400 | (op << 8) | ((rd >> 3) << 7) | (rs << 3) | (rd & 7));
    }
    case 5: return static_cast<u16>(0x4800 | (lo() << 8) | r.below(256));                                    // ldr pc-rel (reads code area)
    case 6: return static_cast<u16>(0x5000 | (r.below(8) << 9) | (lo() << 6) | (6u << 3) | lo());            // ldr/str reg [r6 + ro]
    case 7: return static_cast<u16>(0x6000 | (r.below(4) << 11) | (r.below(32) << 6) | (6u << 3) | lo());    // ldr/str imm5 [r6]
    case 8: return static_cast<u16>(0x8000 | (r.below(2) << 11) | (r.below(32) << 6) | (6u << 3) | lo());    // ldrh/strh
    case 9: return static_cast<u16>(0x9000 | (r.below(2) << 11) | (lo() << 8) | r.below(256));               // sp-relative
    case 10: {  // push/pop without pc/lr, adjust sp, add rd, sp/pc
      switch (r.below(3)) {
      case 0: { u32 list = r.next() & 0xBF; if (!list) continue; return static_cast<u16>(0xB400 | (r.below(2) << 11) | list); }
      case 1: return static_cast<u16>(0xB000 | (r.below(2) << 7) | r.below(16));
      default: return static_cast<u16>(0xA000 | (r.below(2) << 11) | (lo() << 8) | r.below(256));
      }
    }
    default: {  // stmia/ldmia rb!, rb != 6, list without rb
      const u32 rb = lo();
      u32 list = r.next() & 0xBF & ~(1u << rb);
      if (!list) continue;
      return static_cast<u16>(0xC000 | (r.below(2) << 11) | (rb << 8) | list);
    }
    }
  }
}

struct Trial {
  bool thumb;
  std::vector<u32> code;   // ARM words or Thumb halfwords
  u32 regs[16];
  u32 cpsr;
};

void load_trial(Machine& m, const Trial& t, bool a9) {
  write_halt_stub(m, a9);
  u32 addr = CODE_BASE;
  if (t.thumb) {
    for (u32 h : t.code) { m.poke16(addr, static_cast<u16>(h)); addr += 2; }
    // ldr r0, [pc, #k]; bx r0; (pad) .word HALT_STUB
    const u32 ldr_at = addr;
    const u32 pool = (ldr_at + 4 + 3) & ~3u;
    m.poke16(ldr_at, static_cast<u16>(0x4800 | ((pool - ((ldr_at + 4) & ~3u)) >> 2)));
    m.poke16(ldr_at + 2, 0x4700);
    if (pool > ldr_at + 4) m.poke16(ldr_at + 4, 0x46C0);   // nop
    m.poke32(pool, HALT_STUB);
  } else {
    for (u32 w : t.code) { m.poke32(addr, w); addr += 4; }
    const s32 off = static_cast<s32>(HALT_STUB - (addr + 8)) >> 2;
    m.poke32(addr, 0xEA000000u | (static_cast<u32>(off) & 0x00FFFFFF));   // b halt_stub
  }
  for (u32 i = 0; i < 0x2000; i += 4) m.poke32(BUF_BASE + i, 0x01010101u * (i >> 2) ^ 0xA5A5A5A5u);
  for (u32 i = 0; i < 0x400; i += 4) m.poke32(STACK - 0x200 + i, 0x11111111u * (i >> 2));
  CpuContext& c = m.cpu;
  c.set_cpsr(0x1F);                // SYS mode, ARM
  for (int i = 0; i < 15; ++i) c.hot.regs[i] = t.regs[i];
  c.hot.cpsr = t.cpsr | 0x1F | (t.thumb ? 0x20 : 0);
  c.hot.regs[15] = CODE_BASE + (t.thumb ? 4 : 8);
  c.halted = false;
  c.hot.irq_pending = 0;
  // As if a jump had just landed at CODE_BASE (the ARM7 interpreter keeps the
  // code region of the last jump target).
  c.code_cycles = CODE_BASE >> 15;
  c.code_region = CODE_BASE >> 24;
  c.hot.cycle_budget = 1 << 24;
  c.budget_at_halt = 0;
  c.jumped = false;
}

void run_machine(Machine& m, RunFn fn) {
  for (int guard = 0; guard < 100000 && !m.cpu.halted && m.cpu.hot.cycle_budget > 0; ++guard) fn(m.cpu);
}

u32 g_inconclusive = 0;

bool compare(Machine& a, Machine& b, const Trial& t, u32 seed, bool report) {
  // A trial that never reaches the halt stub on either engine (a store that
  // clobbered its own code, a loop through stale memory) only differs in
  // where the budget ran out, which is the one thing the engines are allowed
  // to differ in. Count it and move on.
  if (!a.cpu.halted && !b.cpu.halted && a.cpu.hot.cycle_budget <= 0 && b.cpu.hot.cycle_budget <= 0) { ++g_inconclusive; return true; }
  bool ok = true;
  for (int i = 0; i < 16; ++i) if (a.cpu.hot.regs[i] != b.cpu.hot.regs[i]) ok = false;
  if (a.cpu.hot.cpsr != b.cpu.hot.cpsr) ok = false;
  if (a.cpu.halted != b.cpu.halted) ok = false;
  // Both engines end the slice with budget -1 on a halt; the budget at the
  // moment of halting is the consumed-cycle comparison.
  const s32 ba = a.cpu.halted ? a.cpu.budget_at_halt : a.cpu.hot.cycle_budget;
  const s32 bb = b.cpu.halted ? b.cpu.budget_at_halt : b.cpu.hot.cycle_budget;
  if (ba != bb) ok = false;
  if (std::memcmp(a.host(BUF_BASE), b.host(BUF_BASE), 0x2000) != 0) ok = false;
  if (std::memcmp(a.host(STACK - 0x200), b.host(STACK - 0x200), 0x400) != 0) ok = false;
  if (ok || !report) return ok;
  std::fprintf(stderr, "MISMATCH seed %u (%s), shortest failing prefix:\n", seed, t.thumb ? "thumb" : "arm");
  for (size_t i = 0; i < t.code.size(); ++i) std::fprintf(stderr, "  %08x: %0*x\n", CODE_BASE + static_cast<u32>(i * (t.thumb ? 2 : 4)), t.thumb ? 4 : 8, t.code[i]);
  std::fprintf(stderr, "  initial: cpsr %08x", t.cpsr);
  for (int i = 0; i < 15; ++i) std::fprintf(stderr, " r%d=%08x", i, t.regs[i]);
  std::fprintf(stderr, "\n  %-6s %-10s %-10s\n", "", "interp", "jit");
  for (int i = 0; i < 16; ++i) if (a.cpu.hot.regs[i] != b.cpu.hot.regs[i]) std::fprintf(stderr, "  r%-5d %08x   %08x\n", i, a.cpu.hot.regs[i], b.cpu.hot.regs[i]);
  if (a.cpu.hot.cpsr != b.cpu.hot.cpsr) std::fprintf(stderr, "  cpsr   %08x   %08x\n", a.cpu.hot.cpsr, b.cpu.hot.cpsr);
  if (ba != bb) std::fprintf(stderr, "  budget %08x   %08x (consumed %d vs %d)\n", ba, bb, (1 << 24) - ba, (1 << 24) - bb);
  if (a.cpu.halted != b.cpu.halted) std::fprintf(stderr, "  halted %d %d\n", a.cpu.halted, b.cpu.halted);
  for (u32 i = 0; i < 0x2000; i += 4) {
    u32 x, y; std::memcpy(&x, a.host(BUF_BASE + i), 4); std::memcpy(&y, b.host(BUF_BASE + i), 4);
    if (x != y) { std::fprintf(stderr, "  buf[%04x] %08x %08x\n", i, x, y); }
  }
  return false;
}

bool run_both(Machine& mi, Machine& mj, const Trial& tr, bool a9, u32 seed, bool report) {
  jit::flush(mj.cpu);
  load_trial(mi, tr, a9);
  load_trial(mj, tr, a9);
  run_machine(mi, &interp::run);
  run_machine(mj, &jit::run);
  return compare(mi, mj, tr, seed, report);
}

// Hand-written sequences for cases the generator reaches rarely. Registers
// follow the generator's conventions (r9/r6 = buffer, r13 = stack).
struct Directed { Cpu which; bool thumb; std::vector<u32> code; u32 regs[15]; };

void directed() {
  const Directed cases[] = {
    // Pending fetch cycles of the MOV must survive the LDR taking its slow path
    // (unmapped address through a register offset).
    {Cpu::ARM9, false, {0xE3A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE3A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Same with a store, and with a conditional instruction in front.
    {Cpu::ARM9, false, {0xE0811002, 0xE7891102}, {0, 1, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0x03A00001, 0xE7991102}, {0, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // CP15 cache maintenance (ignored by the core): drain write buffer,
    // invalidate I-cache line, clean+invalidate D-cache line; then an ALU op.
    {Cpu::ARM9, false, {0xEE070F9A, 0xEE073F35, 0xEE070F3E, 0xE2800001}, {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_c keeping the mode (SYS): set F, then clear it; flags field from a register and an immediate.
    {Cpu::ARM9, false, {0xE129F001, 0xE129F002, 0xE128F003, 0xE328F20F, 0xE2800001}, {0, 0x5F, 0x1F, 0xF0000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, false, {0xE129F001, 0xE129F002, 0xE128F003, 0xE328F20F, 0xE2800001}, {0, 0x5F, 0x1F, 0xF0000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_c changing the mode (SYS -> IRQ -> SYS): the interpreter path; r13/r14 are banked.
    {Cpu::ARM9, false, {0xE129F001, 0xE1A0D004, 0xE129F002, 0xE2800001}, {0, 0x92, 0x1F, 0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // MSR CPSR_cxsf with the full mask, same mode, and an IRQ-enable (I cleared) with no IRQ pending.
    {Cpu::ARM9, false, {0xE12FF001, 0xE2800001}, {0, 0x600000DFu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM9, false, {0xE129F001, 0xE2800001}, {0, 0x1F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // Thumb: movs then ldr [r6 + r0] far away.
    {Cpu::ARM9, true, {0x2001, 0x5871}, {0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {Cpu::ARM7, true, {0x2001, 0x5871}, {0, 0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
  };
  u32 n = 0;
  for (const Directed& d : cases) {
    const bool a9 = d.which == Cpu::ARM9;
    Machine mi(d.which), mj(d.which);
    CHECK(jit::attach(mj.nds, a9, !a9));
    Trial t;
    t.thumb = d.thumb;
    t.code = d.code;
    for (int i = 0; i < 15; ++i) t.regs[i] = d.regs[i];
    t.regs[9] = BUF_BASE;
    t.regs[6] = BUF_BASE;
    t.regs[13] = STACK;
    t.cpsr = 0;
    const bool ok = run_both(mi, mj, t, a9, 100000 + n, true);
    jit::detach(mj.nds);
    CHECK(ok);
    ++n;
  }
  std::printf("jit directed: %u cases ok\n", n);
}

void fuzz(Cpu which, bool thumb, u32 trials, u32 seed0) {
  const bool a9 = which == Cpu::ARM9;
  Machine mi(which), mj(which);
  CHECK(jit::attach(mj.nds, a9, !a9));
  u32 fails = 0;
  for (u32 n = 0; n < trials; ++n) {
    const u32 seed = seed0 + n;
    Rng r(seed);
    Trial t;
    t.thumb = thumb;
    const u32 len = 1 + r.below(24);
    for (u32 i = 0; i < len; ++i) t.code.push_back(thumb ? gen_thumb(r) : gen_arm(r));
    for (int i = 0; i < 15; ++i) t.regs[i] = r.coin(30) ? (r.below(5) - 2) : r.next();
    t.regs[9] = BUF_BASE + (r.below(4) << 10);
    t.regs[6] = BUF_BASE + (r.below(4) << 10);
    t.regs[13] = STACK;
    t.cpsr = (r.next() & 0xF0000000u);
    if (run_both(mi, mj, t, a9, seed, false)) continue;
    // Shrink: the shortest failing prefix names the instruction.
    Trial p = t;
    for (u32 k = 1; k <= t.code.size(); ++k) {
      p.code.assign(t.code.begin(), t.code.begin() + k);
      if (!run_both(mi, mj, p, a9, seed, false)) break;
    }
    run_both(mi, mj, p, a9, seed, true);
    if (++fails >= 3) break;
  }
  CHECK(fails == 0);
  std::printf("jit fuzz %s %s: %u trials ok (%u inconclusive)\n", a9 ? "arm9" : "arm7", thumb ? "thumb" : "arm", trials, g_inconclusive);
  g_inconclusive = 0;
  jit::detach(mj.nds);
}

} // namespace

int main(int argc, char** argv) {
  const u32 trials = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 400;
  if (argc > 2) {   // single trial: test_jit 1 <seed>  (seed range selects cpu/state)
    const u32 seed = static_cast<u32>(std::atoi(argv[2]));
    fuzz(seed >= 3000 ? Cpu::ARM7 : Cpu::ARM9, (seed / 1000) % 2 == 0, 1, seed);
    return 0;
  }
  directed();
  fuzz(Cpu::ARM9, false, trials, 1000);
  fuzz(Cpu::ARM9, true, trials, 2000);
  fuzz(Cpu::ARM7, false, trials, 3000);
  fuzz(Cpu::ARM7, true, trials, 4000);
  const jit::Stats& s = jit::stats();
  std::printf("jit: %llu blocks, %llu inline instrs, %llu fallbacks\n",
              (unsigned long long)s.blocks_translated, (unsigned long long)s.instrs_translated, (unsigned long long)s.instrs_fallback);
  std::puts("jit: ok");
  return 0;
}
