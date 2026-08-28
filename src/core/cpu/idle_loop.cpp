// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/idle_loop.h"
#include "core/cpu/arm_decode.h"
#include "core/cpu/cpu.h"

#include <cstdlib>
#include <cstring>

namespace ds::cpu {

namespace {

constexpr u32 MAX_BODY   = 16;    // instructions in the loop body
constexpr u32 MAX_SEARCH = 16;    // instructions scanned forward for the back edge
constexpr u32 MAX_LOADS  = 4;     // load sites re-checked per query
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
  u32  head = 0, tail = 0;
  u32  checksum = 0;       // body words, re-verified per query (SMC / remap)
  u32  load_count = 0;
  LoadSite loads[MAX_LOADS];
};

// Direct-mapped structural cache, one per CPU. Keyed by PC; a collision just
// re-analyses, which is why no eviction policy is needed.
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

// A poll loop nearly always reads a device register, so refusing all MMIO
// refuses the whole point. Under the caller's guard -- no DMA, geometry idle,
// sibling CPU idle, no pending IRQ -- no device can change state before the
// next scheduled event, and the slice ends there. So the value a register
// returns is the same on every iteration and reading it is pure, with two
// exceptions whose reads have side effects and must never be skipped over:
// the IPC receive FIFO and the gamecard data port, which both pop.
inline bool safe_poll_address(CpuContext& cpu, u32 addr, IdlePorts ports, bool& gxstat) {
  if (cpu.page_table.read_ptr(addr)) return true;          // plain RAM
  if ((addr & 0x0F000000u) != 0x04000000u) return false;   // not I/O at all
  const u32 port = addr & 0x0FFFFFFCu;
  if (ports != IdlePorts::All) {
    if (ports == IdlePorts::GxstatOnly && port == 0x04000600) { gxstat = true; return true; }
    return false;
  }
  // Only registers that change at a scheduled event, never between two of
  // them. Anything the scheduler derives from the current time (VCOUNT,
  // DISPSTAT's blank bits, the timer counters) advances *inside* a slice --
  // Scheduler::now() interpolates from the running CPU's consumed budget --
  // so a loop polling those observes values a skip would never produce.
  // Anything whose read pops a queue is excluded for the obvious reason, and
  // so are the cartridge/save ports (AUXSPI, ROMCTRL): their busy bits are
  // driven by transfer timing, and skipping past them changed output in
  // Bowser's Inside Story and Meteos.
  // DS_IDLE_PORTS=<hex>,<hex>,... overrides the set, for bisecting which
  // register a divergence comes from.
  static const char* env = std::getenv("DS_IDLE_PORTS");
  if (env) {
    for (const char* p = env; *p;) {
      const u32 v = static_cast<u32>(std::strtoul(p, const_cast<char**>(&p), 16));
      if (v == port) return true;
      while (*p == ',' || *p == ' ') ++p;
      if (!*p) break;
    }
    return false;
  }
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

// Instruction fetch through the page table: an unmapped or MMIO page is not
// code we are willing to reason about.
inline bool fetch(CpuContext& cpu, u32 addr, u32& out) {
  const u8* p = cpu.page_table.read_ptr(addr);
  if (!p) return false;
  std::memcpy(&out, p, 4);
  return true;
}

// Sum of the body's instruction words. Recomputed on every query so that
// modified code or a remapped page falls back to a fresh analysis instead of
// trusting a stale verdict; the body is at most MAX_BODY words, and a query
// happens once per slice, not once per instruction.
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
  u32 load_count = 0;
  LoadSite loads[MAX_LOADS];
  bool ok = true;
};

inline void do_read(BodyScan& s, u32 r) {
  if (!(s.written & (1u << r))) s.read_before_write |= 1u << r;
}
inline void do_write(BodyScan& s, u32 r) { s.written |= 1u << r; }

// Sign-extend a 24-bit branch offset and resolve it against the ARM pipeline.
inline u32 branch_target(u32 at, u32 instr) {
  s32 imm = static_cast<s32>(instr << 8) >> 6;   // sign-extend imm24, times 4
  return at + 8 + static_cast<u32>(imm);
}

// Classify one instruction of the candidate body. Returns false to reject the
// whole loop. `at` is the instruction's own address.
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
    const bool moves = opcode == 0xD || opcode == 0xF;        // MOV MVN: rn unused
    if (!moves) do_read(s, rn);
    if (op != AOp::DpImm) do_read(s, rm);
    if (op == AOp::DpRegShift) do_read(s, rs);
    if (opcode == 0x5 || opcode == 0x6 || opcode == 0x7) do_read(s, FLAGS);  // ADC SBC RSC
    if (compare) { do_write(s, FLAGS); break; }
    if (rd == 15) return reject(IdleReject::PcWrite);                               // a computed jump is not a poll loop
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
    // An unconditional branch that leaves the range means this is not a loop.
    if (cond == 0xE && !inside && at != tail) return reject(IdleReject::BadInstr);
    break;   // conditional exits are fine: the exit condition cannot change while we skip
  }
  default:
    // stores, LDM/STM, SWP, SWI, PSR writes, coprocessor, calls, undefined
    return reject((op == AOp::Stm || op == AOp::LdrStrReg || op == AOp::LdrStrHReg)
                    ? IdleReject::Store : IdleReject::BadInstr);
  }
  return true;
}

// Analyse the loop that contains `pc`, if any, and fill `v`.
void analyse(CpuContext& cpu, u32 pc, Verdict& v) {
  ++g_stats.analyses;
  v.pc = pc;
  v.valid = true;
  v.skippable = false;
  v.load_count = 0;

  // Find the back edge: scan forward for a branch that jumps to `pc` or just
  // above it, i.e. the bottom of a loop that contains `pc`.
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
  // A loop that carries a register across the back edge makes progress on its
  // own -- a delay loop, not a poll loop.
  if (s.written & s.read_before_write) { reject(IdleReject::Carried); return; }
  // A loop with no load can never observe a change, so it either spins forever
  // or is not what we think it is; either way, leave it alone.
  if (s.load_count == 0) { reject(IdleReject::NoLoad); return; }

  v.skippable = true;
  v.head = head;
  v.tail = tail;
  v.checksum = body_checksum(cpu, head, tail);
  v.load_count = s.load_count;
  for (u32 i = 0; i < s.load_count; ++i) v.loads[i] = s.loads[i];
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
  if (!v.valid || v.pc != pc || stale) analyse(cpu, pc, v);
  if (!v.skippable) return false;

  // Re-check every load against the live registers: the same code may run with
  // a base pointer into MMIO, where the read itself can have a side effect.
  bool gxstat = false;
  for (u32 i = 0; i < v.load_count; ++i) {
    const LoadSite& site = v.loads[i];
    const u32 addr = site.base == 0xFF
                       ? site.addr
                       : cpu.hot.regs[site.base] + static_cast<u32>(site.offset);
    if (!safe_poll_address(cpu, addr, ports, gxstat)) return reject(IdleReject::Mmio);
  }
  // The swap-wait shape is a loop *on GXSTAT*; a RAM-only loop that happens
  // to run while a swap is pending is not what this mode is for.
  if (ports == IdlePorts::GxstatOnly && !gxstat) return reject(IdleReject::Mmio);
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
  }
  return "?";
}

}  // namespace ds::cpu
