// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/idle_loop.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::cpu {

namespace {

constexpr u32 MAX_BODY   = 16;    // instructions in the loop body
constexpr u32 MAX_SEARCH = 16;    // instructions scanned forward for the back edge
constexpr u32 MAX_LOADS  = 4;     // load sites re-checked per query
constexpr u32 MAX_PROOF  = 8;     // words a proven lap may read and still be remembered
constexpr u32 FLAGS      = 16;    // pseudo-register for the condition flags

struct LoadSite {
  u8  base;        // base register, or 0xFF when `addr` is already absolute
  s32 offset;
  u32 addr;        // resolved address for PC-relative loads
};

struct Verdict {
  u32  pc = 0;             // key: address of the instruction queried
  bool valid = false;      // an entry lives here
  bool skippable = false;  // ... and it proved a pure loop
  IdleReject reason = IdleReject::None;   // so a cached rejection reports the same reason
  u32  head = 0, tail = 0;
  u32  checksum = 0;       // body words, re-verified per query (SMC / remap)
  u32  load_count = 0;
  LoadSite loads[MAX_LOADS];
  // The last state still_spinning proved: the registers and flags at `pc`, and
  // the words its loads read. The same inputs give the same lap, so a query
  // that finds them unchanged needs no second run. proof_n 0xFF: none.
  u32 reads = 0;           // registers (+FLAGS) the body reads: what a lap depends on
  u8  proof_n = 0xFF;
  IdlePorts proof_ports = IdlePorts::All;
  u32 proof_regs[15], proof_cpsr = 0;
  u32 proof_addr[MAX_PROOF], proof_word[MAX_PROOF];
};

// Direct-mapped structural cache, one per CPU. A collision just re-analyses.
constexpr u32 CACHE_BITS = 9;
constexpr u32 CACHE_SIZE = 1u << CACHE_BITS;
Verdict g_cache[2][CACHE_SIZE];
IdleLoopStats g_stats;
IdleReject g_reject = IdleReject::None;
inline bool reject(IdleReject r) {
  g_reject = r;
  ++g_stats.by_reason[static_cast<u32>(r)];
  return false;
}

inline u32 cache_slot(u32 pc) { return (pc >> 2) & (CACHE_SIZE - 1); }

// Under the caller's guard (no DMA, geometry idle, sibling idle, no pending
// IRQ) no device changes state before the next scheduled event, so a register
// read is pure -- except the IPC receive FIFO and gamecard data port, which pop.
inline bool safe_poll_address(CpuContext& cpu, u32 addr, IdlePorts ports, bool& gxstat) {
  if (cpu.page_table.read_ptr(addr)) return true;          // plain RAM
  if ((addr & 0x0F000000u) != 0x04000000u) return false;   // not I/O at all
  const u32 port = addr & 0x0FFFFFFCu;
  if (ports != IdlePorts::All) {
    if (ports == IdlePorts::GxstatOnly && port == 0x04000600) { gxstat = true; return true; }
    if (ports == IdlePorts::SpicntOnly && port == 0x040001C0) return true;
    return false;
  }
  // Only registers that change at a scheduled event, never inside a slice
  // (excludes VCOUNT/DISPSTAT/timers, which Scheduler::now() interpolates,
  // and AUXSPI/ROMCTRL, whose busy bits follow transfer timing).
  switch (port) {
  case 0x04000180:   // IPCSYNC
  case 0x04000184:   // IPCFIFOCNT
  case 0x04000208:   // IME
  case 0x0400020C:   // IE (high half)
  case 0x04000210:   // IE
  case 0x04000214:   // IF
  case 0x04000600:   // GXSTAT
    return true;
  default:
    return false;
  }
}

inline bool fetch(CpuContext& cpu, u32 addr, u32& out) {
  const u8* p = cpu.page_table.read_ptr(addr);
  if (!p) return false;
  std::memcpy(&out, p, 4);
  return true;
}

// Recomputed on every query so SMC or a remapped page falls back to a fresh
// analysis; at most MAX_BODY words, once per slice.
u32 body_checksum(CpuContext& cpu, u32 head, u32 tail) {
  u32 sum = 0x9E3779B9u;
  for (u32 at = head; at <= tail; at += 4) {
    u32 instr;
    if (!fetch(cpu, at, instr)) return 0;
    sum = (sum ^ instr) * 16777619u;
  }
  return sum ? sum : 1u;
}

struct BodyScan {
  u32 written = 0;            // registers (+FLAGS) the body writes
  u32 read_before_write = 0;  // ... and reads before any write in the same iteration
  u32 reads = 0;              // ... and reads at all
  u32 load_count = 0;
  LoadSite loads[MAX_LOADS];
  bool ok = true;
};

inline void do_read(BodyScan& s, u32 r) {
  s.reads |= 1u << r;
  if (!(s.written & (1u << r))) s.read_before_write |= 1u << r;
}
inline void do_write(BodyScan& s, u32 r) { s.written |= 1u << r; }

inline u32 branch_target(u32 at, u32 instr) {
  s32 imm = static_cast<s32>(instr << 8) >> 6;   // sign-extend imm24, times 4
  return at + 8 + static_cast<u32>(imm);
}

// Classify one instruction of the candidate body. Returns false to reject the loop.
bool scan_instr(BodyScan& s, u32 at, u32 instr, u32 head, u32 tail) {
  using arm::AOp;
  const u32 cond = instr >> 28;
  if (cond == 0xF) return reject(IdleReject::BadInstr);   // unconditional-space encodings (BLX, PLD, ...)
  if (cond != 0xE) do_read(s, FLAGS);            // a predicated instruction reads the flags

  const AOp op = arm::classify_arm(arm::arm_index(instr));
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  const u32 rm = instr & 0xF, rs = (instr >> 8) & 0xF;

  switch (op) {
  case AOp::DpImm:
  case AOp::DpImmShift:
  case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool sets_flags = (instr >> 20) & 1;
    const bool compare = opcode >= 0x8 && opcode <= 0xB;      // TST TEQ CMP CMN
    const bool moves = opcode == 0xD || opcode == 0xF;        // MOV MVN
    if (!moves) do_read(s, rn);
    if (op != AOp::DpImm) do_read(s, rm);
    if (op == AOp::DpRegShift) do_read(s, rs);
    if (opcode == 0x5 || opcode == 0x6 || opcode == 0x7) do_read(s, FLAGS);  // ADC SBC RSC
    if (compare) { do_write(s, FLAGS); break; }
    if (rd == 15) return reject(IdleReject::PcWrite);   // a computed jump is not a poll loop
    do_write(s, rd);
    if (sets_flags) do_write(s, FLAGS);
    break;
  }
  case AOp::Mrs:
    if (rd == 15) return reject(IdleReject::PcWrite);
    do_write(s, rd);
    break;
  case AOp::Clz:
    if (rd == 15) return reject(IdleReject::PcWrite);
    do_read(s, rm); do_write(s, rd);
    break;
  case AOp::Mul: case AOp::Mla: {
    // rd and rn are swapped in the multiply encoding.
    const u32 mrd = rn, macc = rd;
    if (mrd == 15) return reject(IdleReject::PcWrite);
    do_read(s, rm); do_read(s, rs);
    if (op == AOp::Mla) do_read(s, macc);
    do_write(s, mrd);
    if ((instr >> 20) & 1) do_write(s, FLAGS);
    break;
  }
  case AOp::LdrStrImm:
  case AOp::LdrStrHImm: {
    const bool load = (instr >> 20) & 1;
    const bool pre  = (instr >> 24) & 1;
    const bool wb   = (instr >> 21) & 1;
    const bool up   = (instr >> 23) & 1;
    if (!load) return reject(IdleReject::Store);            // a store is observable
    if (!pre || wb) return reject(IdleReject::LoadForm);   // writeback moves the base between iterations
    if (rd == 15) return reject(IdleReject::PcWrite);
    s32 imm;
    if (op == AOp::LdrStrImm) imm = static_cast<s32>(instr & 0xFFF);
    else                      imm = static_cast<s32>(((instr >> 4) & 0xF0) | (instr & 0xF));
    if (!up) imm = -imm;
    if (s.load_count >= MAX_LOADS) return false;
    LoadSite& site = s.loads[s.load_count++];
    if (rn == 15) { site.base = 0xFF; site.addr = at + 8 + static_cast<u32>(imm); site.offset = 0; }
    else          { site.base = static_cast<u8>(rn); site.offset = imm; site.addr = 0; do_read(s, rn); }
    do_write(s, rd);
    break;
  }
  case AOp::B: {
    const u32 target = branch_target(at, instr);
    const bool inside = target >= head && target <= tail;
    if (cond == 0xE && !inside && at != tail) return reject(IdleReject::BadInstr);
    break;   // conditional exits are fine: the exit condition cannot change while we skip
  }
  default:
    return reject((op == AOp::Stm || op == AOp::LdrStrReg || op == AOp::LdrStrHReg)
                    ? IdleReject::Store : IdleReject::BadInstr);
  }
  return true;
}

void analyse(CpuContext& cpu, u32 pc, Verdict& v) {
  ++g_stats.analyses;
  v.pc = pc;
  v.valid = true;
  v.skippable = false;
  v.load_count = 0;
  v.proof_n = 0xFF;

  // Scan forward for a branch that jumps to `pc` or just above it: the
  // bottom of a loop containing `pc`.
  u32 head = 0, tail = 0;
  bool found = false;
  for (u32 i = 0; i < MAX_SEARCH; ++i) {
    const u32 at = pc + i * 4;
    u32 instr;
    if (!fetch(cpu, at, instr)) { reject(IdleReject::Fetch); return; }
    const u32 cond = instr >> 28;
    if (cond == 0xF) { reject(IdleReject::BadInstr); return; }
    if (arm::classify_arm(arm::arm_index(instr)) != arm::AOp::B) continue;
    const u32 target = branch_target(at, instr);
    if (target <= pc && pc - target <= MAX_BODY * 4) { head = target; tail = at; found = true; break; }
  }
  if (!found) { reject(IdleReject::NoBackEdge); return; }
  if ((tail - head) / 4 + 1 > MAX_BODY) { reject(IdleReject::TooLong); return; }

  BodyScan s;
  for (u32 at = head; at <= tail; at += 4) {
    u32 instr;
    if (!fetch(cpu, at, instr)) { reject(IdleReject::Fetch); return; }
    if (!scan_instr(s, at, instr, head, tail)) { return; }
  }
  // Carrying a register across the back edge is a delay loop, not a poll loop.
  if (s.written & s.read_before_write) { reject(IdleReject::Carried); return; }
  if (s.load_count == 0) { reject(IdleReject::NoLoad); return; }

  v.skippable = true;
  v.head = head;
  v.tail = tail;
  v.checksum = body_checksum(cpu, head, tail);
  v.load_count = s.load_count;
  v.reads = s.reads;
  for (u32 i = 0; i < s.load_count; ++i) v.loads[i] = s.loads[i];
}

// The proof that the loop is going nowhere *now*: run it from the live state
// through the interpreter until it comes back to `pc`, and require the
// registers and flags it comes back with to be the ones it left with. The body
// has only pure instructions and every load is checked against the address it
// really uses, so a state that repeats keeps repeating until memory or a
// register a scheduled event owns changes. A lap that ends elsewhere (the
// CPU entered mid-body from other code) gets one more; an exit branch taken,
// or a load from a device, is not idle. The CPU is put back as it was.
// One instruction of a lap, for the forms poll loops are made of: immediate
// and shifted-register data processing, and aligned LDR / LDRB / LDRH. r15
// holds the instruction's address + 8 and the condition has passed. False:
// not one of these, nothing changed, the interpreter runs it. (A proof runs
// dozens of times a frame from the scheduler; a trip through the interpreter,
// cold under the recompiler, cost Spirit Tracks 0.2 ms a frame.)
bool quick_step(CpuContext& cpu, arm::AOp op, u32 instr) {
  using arm::AOp;
  u32* r = cpu.hot.regs;
  const u32 rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
  if (op == AOp::LdrStrImm || op == AOp::LdrStrHImm) {
    s32 imm = op == AOp::LdrStrImm ? static_cast<s32>(instr & 0xFFF) : static_cast<s32>(((instr >> 4) & 0xF0) | (instr & 0xF));
    if (!(instr & (1u << 23))) imm = -imm;
    const u32 addr = r[rn] + static_cast<u32>(imm);
    const u32 size = op == AOp::LdrStrHImm ? (((instr >> 5) & 3) == 1 ? 2u : 0u) : (instr & (1u << 22)) ? 1u : 4u;
    if (!size || (addr & (size - 1))) return false;
    u32 v = 0;
    if (const u8* p = cpu.page_table.read_ptr(addr)) std::memcpy(&v, p, size);
    else v = size == 4 ? cpu.nds->bus.read32(cpu.which, addr) : size == 2 ? cpu.nds->bus.read16(cpu.which, addr) : cpu.nds->bus.read8(cpu.which, addr);
    r[rd] = v;
    return true;
  }
  if (op != AOp::DpImm && op != AOp::DpImmShift) return false;
  const u32 cpsr = cpu.hot.cpsr;
  const u32 c_in = (cpsr >> 29) & 1;
  u32 b, sc = c_in;   // second operand and the shifter's carry out
  if (op == AOp::DpImm) {
    const u32 rot = ((instr >> 8) & 0xF) * 2, imm = instr & 0xFF;
    b = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
    if (rot) sc = b >> 31;
  } else {
    const u32 m = r[instr & 0xF], amt = (instr >> 7) & 0x1F;
    switch ((instr >> 5) & 3) {
    case 0:  b = m << amt; if (amt) sc = (m >> (32 - amt)) & 1; break;
    case 1:  b = amt ? m >> amt : 0; sc = (m >> ((amt ? amt : 32) - 1)) & 1; break;
    case 2:  b = static_cast<u32>(static_cast<s32>(m) >> (amt ? amt : 31)); sc = (m >> ((amt ? amt : 32) - 1)) & 1; break;
    default: if (amt) { b = (m >> amt) | (m << (32 - amt)); sc = b >> 31; } else { b = (c_in << 31) | (m >> 1); sc = m & 1; } break;
    }
  }
  const u32 a = r[rn], opcode = (instr >> 21) & 0xF;
  u32 res, c = sc, v = (cpsr >> 28) & 1;
  const auto add = [&](u32 x, u32 y, u32 carry) {
    const u64 sum = static_cast<u64>(x) + y + carry;
    res = static_cast<u32>(sum); c = static_cast<u32>(sum >> 32); v = (~(x ^ y) & (x ^ res)) >> 31;
  };
  switch (opcode) {
  case 0x0: case 0x8: res = a & b; break;
  case 0x1: case 0x9: res = a ^ b; break;
  case 0x2: case 0xA: add(a, ~b, 1); break;
  case 0x3: add(b, ~a, 1); break;
  case 0x4: case 0xB: add(a, b, 0); break;
  case 0x5: add(a, b, c_in); break;
  case 0x6: add(a, ~b, c_in); break;
  case 0x7: add(b, ~a, c_in); break;
  case 0xC: res = a | b; break;
  case 0xD: res = b; break;
  case 0xE: res = a & ~b; break;
  default:  res = ~b; break;
  }
  if (opcode < 0x8 || opcode > 0xB) r[rd] = res;   // the scan turned away rd = 15 here
  if (instr & (1u << 20)) cpu.hot.cpsr = (cpsr & 0x0FFFFFFFu) | (res & 0x80000000u) | (res ? 0 : 0x40000000u) | (c << 29) | (v << 28);
  return true;
}

inline u32 peek_word(CpuContext& cpu, u32 addr) {
  addr &= ~3u;
  if (const u8* p = cpu.page_table.read_ptr(addr)) { u32 w; std::memcpy(&w, p, 4); return w; }
  return cpu.nds->bus.read32(cpu.which, addr);   // one of safe_poll_address's registers: a pure read
}

bool still_spinning(CpuContext& cpu, Verdict& v, u32 pc, IdlePorts ports) {
  // Only what the body reads decides a lap: a register an interrupt handler
  // left different, or flags no instruction tests, do not undo the proof.
  const u32 cpsr_mask = (v.reads & (1u << FLAGS)) ? ~0u : 0x0FFFFFFFu;
  bool known = v.proof_n != 0xFF && v.proof_ports == ports && !((v.proof_cpsr ^ cpu.hot.cpsr) & cpsr_mask);
  for (u32 r = 0; known && r < 15; ++r) if ((v.reads & (1u << r)) && v.proof_regs[r] != cpu.hot.regs[r]) known = false;
  if (known) {
    u32 i = 0;
    while (i < v.proof_n && peek_word(cpu, v.proof_addr[i]) == v.proof_word[i]) ++i;
    if (i == v.proof_n) return true;
  }
  v.proof_n = 0xFF;
  // DS_IDLE_CHECK=1: every instruction through the interpreter too, compared.
  static const bool check = [] { const char* e = std::getenv("DS_IDLE_CHECK"); return e && e[0] == '1'; }();
  u32 seen_addr[MAX_PROOF], seen_word[MAX_PROOF], seen = 0;
  bool seen_all = true;
  struct Saved {
    JitHot hot; bool jumped; u32 code_cycles, data_cycles, code_region, data_region; u8 fast_d; bool branch_fetch;
  } const saved{cpu.hot, cpu.jumped, cpu.code_cycles, cpu.data_cycles, cpu.code_region, cpu.data_region, cpu.fast_d, cpu.branch_fetch};
  const auto same = [&cpu](const u32* regs, u32 cpsr) {
    return cpu.hot.cpsr == cpsr && !std::memcmp(cpu.hot.regs, regs, 15 * sizeof(u32));
  };
  u32 lap_regs[15]; std::memcpy(lap_regs, cpu.hot.regs, sizeof lap_regs);
  u32 lap_cpsr = cpu.hot.cpsr;
  IdleReject why = IdleReject::None;
  bool gxstat = false, spinning = false;
  u32 at = pc;
  for (u32 lap = 0, steps = 0; lap < 2 && why == IdleReject::None && !spinning; ++steps) {
    u32 instr;
    if (steps > 2 * MAX_BODY || !fetch(cpu, at, instr)) { why = IdleReject::Fetch; break; }
    u32 next = at + 4;
    const arm::AOp op = arm::classify_arm(arm::arm_index(instr));
    const bool runs = arm::check_condition(instr >> 28, cpu.hot.cpsr);
    if (op == arm::AOp::B) {
      if (runs) next = branch_target(at, instr);
    } else {
      if (runs && (op == arm::AOp::LdrStrImm || op == arm::AOp::LdrStrHImm)) {
        const u32 rn = (instr >> 16) & 0xF;
        s32 imm = op == arm::AOp::LdrStrImm ? static_cast<s32>(instr & 0xFFF) : static_cast<s32>(((instr >> 4) & 0xF0) | (instr & 0xF));
        if (!(instr & (1u << 23))) imm = -imm;
        const u32 addr = (rn == 15 ? at + 8 : cpu.hot.regs[rn]) + static_cast<u32>(imm);
        if (!safe_poll_address(cpu, addr, ports, gxstat)) { why = IdleReject::Mmio; break; }
        if (seen < MAX_PROOF) { seen_addr[seen] = addr; seen_word[seen++] = peek_word(cpu, addr); } else seen_all = false;
      }
      cpu.hot.regs[15] = at + 8;
      if (check) {
        const JitHot before = cpu.hot;
        const bool mine = runs && quick_step(cpu, op, instr);
        const JitHot after = cpu.hot;
        cpu.hot = before; cpu.data_cycles = 0; cpu.jumped = false;
        interp::exec_arm(cpu, instr);
        if (mine && (std::memcmp(after.regs, cpu.hot.regs, sizeof after.regs) || after.cpsr != cpu.hot.cpsr) && ++g_stats.check_differs)
          std::fprintf(stderr, "[idle] quick_step differs from the interpreter: %08x at %08x\n", instr, at);
      } else if (runs && !quick_step(cpu, op, instr)) {
        cpu.data_cycles = 0;
        cpu.jumped = false;
        interp::exec_arm(cpu, instr);
      }
    }
    if (next < v.head || next > v.tail) { why = IdleReject::Exit; break; }
    at = next;
    if (at != pc) continue;
    if (same(lap_regs, lap_cpsr)) spinning = true;
    else { std::memcpy(lap_regs, cpu.hot.regs, sizeof lap_regs); lap_cpsr = cpu.hot.cpsr; ++lap; }
  }
  cpu.hot = saved.hot; cpu.jumped = saved.jumped;
  cpu.code_cycles = saved.code_cycles; cpu.data_cycles = saved.data_cycles;
  cpu.code_region = saved.code_region; cpu.data_region = saved.data_region;
  cpu.fast_d = saved.fast_d; cpu.branch_fetch = saved.branch_fetch;
  if (why != IdleReject::None) return reject(why);
  if (!spinning) return reject(IdleReject::Exit);
  // A RAM-only loop that happens to run during a pending swap doesn't count.
  if (ports == IdlePorts::GxstatOnly && !gxstat) return reject(IdleReject::Mmio);
  if (seen_all) {
    std::memcpy(v.proof_regs, saved.hot.regs, sizeof v.proof_regs); v.proof_cpsr = saved.hot.cpsr;
    std::memcpy(v.proof_addr, seen_addr, sizeof seen_addr); std::memcpy(v.proof_word, seen_word, sizeof seen_word);
    v.proof_ports = ports; v.proof_n = static_cast<u8>(seen);
  }
  return true;
}

}  // namespace

bool in_idle_loop(CpuContext& cpu, IdlePorts ports) {
  ++g_stats.queries;
  g_reject = IdleReject::None;
  if (cpu.thumb()) return reject(IdleReject::Thumb);
  const u32 pc = cpu.hot.regs[15] - 8;   // r15 runs one pipeline ahead (see CpuContext::jump)

  Verdict& v = g_cache[cpu.which == Cpu::ARM9 ? 0 : 1][cache_slot(pc)];
  const bool stale = v.valid && v.pc == pc && v.skippable &&
                     body_checksum(cpu, v.head, v.tail) != v.checksum;
  if (!v.valid || v.pc != pc || stale) { analyse(cpu, pc, v); v.reason = g_reject; }
  if (!v.skippable) return reject(v.reason);

  // DS_IDLE_PROOF=0: the structural verdict and the load addresses alone (A/B).
  static const bool proof = [] { const char* e = std::getenv("DS_IDLE_PROOF"); return !e || e[0] != '0'; }();
  // The SPI busy poll keeps the address check alone: its caller has seen the
  // transfer busy and wakes the CPU when it ends, and the proof there cost
  // Spirit Tracks 0.15 ms a frame on the RG DS Plus (2026-11-21).
  if (proof && ports != IdlePorts::SpicntOnly) {
    // An ARM7 register read counts towards (or ends) the SPI poll streak: not the proof's reads.
    const u32 streak = cpu.nds->io.spi_poll_streak_;
    const bool ok = still_spinning(cpu, v, pc, ports);
    cpu.nds->io.spi_poll_streak_ = streak;
    if (!ok) return false;
  } else {
    bool gxstat = false;
    for (u32 i = 0; i < v.load_count; ++i) {
      const LoadSite& site = v.loads[i];
      const u32 addr = site.base == 0xFF ? site.addr : cpu.hot.regs[site.base] + static_cast<u32>(site.offset);
      if (!safe_poll_address(cpu, addr, ports, gxstat)) return reject(IdleReject::Mmio);
    }
    if (ports == IdlePorts::GxstatOnly && !gxstat) return reject(IdleReject::Mmio);
  }
  ++g_stats.hits;
  return true;
}

void invalidate_idle_loops() {
  for (auto& per_cpu : g_cache)
    for (auto& v : per_cpu) v.valid = false;
}

const IdleLoopStats& idle_loop_stats() { return g_stats; }
IdleReject idle_loop_last_reject() { return g_reject; }
const char* idle_reject_name(IdleReject r) {
  switch (r) {
  case IdleReject::None: return "ok";
  case IdleReject::Thumb: return "thumb";
  case IdleReject::Fetch: return "fetch";
  case IdleReject::NoBackEdge: return "no-back-edge";
  case IdleReject::TooLong: return "too-long";
  case IdleReject::Store: return "store";
  case IdleReject::LoadForm: return "load-form";
  case IdleReject::BadInstr: return "bad-instr";
  case IdleReject::PcWrite: return "pc-write";
  case IdleReject::Carried: return "carried-dep";
  case IdleReject::NoLoad: return "no-load";
  case IdleReject::Mmio: return "mmio";
  case IdleReject::Exit: return "exit-met";
  }
  return "?";
}

}  // namespace ds::cpu
