// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/sched/scheduler.h"
#include "core/nds.h"
#include "core/profile.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/idle_loop.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <cstdlib>
#include <limits>
#include <algorithm>

namespace ds {

Scheduler::Scheduler(NDS& nds) : nds_(nds), now_(0) {
  // Read once: a function-local static costs an acquire load per use.
  if (const char* q = std::getenv("DS_QUANTUM")) { set_quantum(std::atoll(q)); quantum_forced_ = true; }
  debug_slices_ = std::getenv("DS_DEBUG_SLICES") != nullptr;
  if (const char* e = std::getenv("DS_IDLE_SKIP"))
    idle_skip_ = (e[0] == '0') ? 0 : (std::strcmp(e, "all") == 0 || e[0] == '2') ? 2 : 1;
  reset();
}

void Scheduler::gx_fifo_full() {
  if (quantum_ <= LOCKSTEP_QUANTUM || in_dma_) return;
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  if (running_ == &a9) preempt(a9);
}

bool Scheduler::a9_gx_stalled(const CpuContext& cpu) const {
  return quantum_ > LOCKSTEP_QUANTUM && cpu.which == Cpu::ARM9 && nds_.gpu3d.stalled();
}

void Scheduler::set_quantum(s64 q) {
  if (quantum_forced_) return;
  quantum_ = q <= 0 ? EVENT_BOUND_QUANTUM : q;
}

void Scheduler::reset() {
  now_ = 0;
  arm7_debt_ = 0;
  armed_ = 0;
  at_.fill(0); fn_.fill(nullptr); param_.fill(0);
  next_ = std::numeric_limits<u64>::max();
  next_id_ = EVENT_COUNT;
}

// `next_` caches the earliest armed deadline so the per-slice loop scans the
// table only when an event is actually due (or after a cancel).
void Scheduler::schedule(EventId id, u64 at, EventFn fn, u32 param) {
  const u32 i = static_cast<u32>(id);
  // Only a *live* next event can be pushed later and leave next_ stale. An
  // event rescheduling itself from its own handler is already disarmed, and
  // fire_due rescans when the pass ends -- rescanning here would double it.
  const bool was_next = (armed_ & (1u << i)) && next_id_ == i;
  at_[i] = at; fn_[i] = fn; param_[i] = param;
  armed_ |= 1u << i;
  if (at < next_) { next_ = at; next_id_ = i; }
  else if (was_next && at > next_) rescan();
}

void Scheduler::cancel(EventId id) {
  const u32 i = static_cast<u32>(id);
  if (!(armed_ & (1u << i))) return;
  armed_ &= ~(1u << i);
  if (next_id_ == i) rescan();
}

// Earliest armed deadline and which event owns it. Only the armed events are
// visited; `next_id_` lets schedule() and cancel() tell "the one that defines
// next_" from "one that merely ties with it", which the old time comparison
// could not.
void Scheduler::rescan() {
  u64 best = std::numeric_limits<u64>::max();
  u32 best_id = EVENT_COUNT;
  for (u32 m = armed_; m; m &= m - 1) {
    const u32 i = static_cast<u32>(__builtin_ctz(m));
    if (at_[i] < best) { best = at_[i]; best_id = i; }
  }
  next_ = best;
  next_id_ = best_id;
}

// A CPU counts as idle when it is halted, or awake but provably going nowhere.
// Skipping is only safe while nothing else can change what the loop reads:
// the sibling CPU must be idle too, no DMA may be running, and the geometry
// engine must be quiet. The slice still ends at the next scheduled event, so
// whatever the loop waits for is delivered on time.
bool Scheduler::machine_idle(bool& skip9, bool& skip7) const {
  skip9 = skip7 = false;
  if (!idle_skip_) return both_idle();
  // Swap-wait mode: nothing below is worth its cost unless the game has issued
  // a swap and is waiting for VBlank to perform it -- one load decides that
  // before the PC ring, the body walk or the DMA probes are touched, so a
  // scene that never waits this way pays only this test per slice.
  const bool gx_only = idle_skip_ == 1;
  if (gx_only && !nds_.gpu3d.swap_pending()) return both_idle();
  CpuContext& a9 = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM9));
  CpuContext& a7 = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM7));
  if (nds_.dma.any_running(Cpu::ARM9) || nds_.dma.any_running(Cpu::ARM7)) { prof::add(prof::C_IDLE_NO_DMA, 1); return false; }
  if (!nds_.gpu3d.idle()) { prof::add(prof::C_IDLE_NO_GX, 1); return false; }

  CpuContext* cpus[2] = {&a9, &a7};
  // Pre-filter: analysing costs a body walk, so only look at a CPU that came
  // back to the same instruction it left on -- what a spinning CPU does.
  // Both slots are refreshed before any early return, so a rejection on one
  // CPU cannot leave the other's filter stale.
  bool repeated[2];
  for (int i = 0; i < 2; ++i) {
    const u32 pc = cpus[i]->hot.regs[15];
    repeated[i] = false;
    for (u32 k = 0; k < 8; ++k) if (idle_pc_ring_[i][k] == pc) { repeated[i] = true; break; }
    idle_pc_ring_[i][idle_pc_pos_[i]++ & 7] = pc;
  }
  bool skip[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    CpuContext& c = *cpus[i];
    // An unmasked pending IRQ means the CPU is about to leave, halted or not.
    if (c.hot.irq_pending && !(c.hot.cpsr & 0x80)) { prof::add(prof::C_IDLE_NO_IRQ, 1); return false; }
    if (c.halted) continue;
    if (!repeated[i]) { prof::add(prof::C_IDLE_NO_FILTER, 1); return false; }
    // Swap-wait mode: the ARM9 loop must read GXSTAT and no other device; the
    // ARM7 (which cannot see GXSTAT) may only spin on RAM.
    const cpu::IdlePorts ports = !gx_only ? cpu::IdlePorts::All : i == 0 ? cpu::IdlePorts::GxstatOnly : cpu::IdlePorts::RamOnly;
    if (!cpu::in_idle_loop(c, ports)) { prof::add(i ? prof::C_IDLE_NO_LOOP7 : prof::C_IDLE_NO_LOOP9, 1); return false; }
    skip[i] = true;
  }
  prof::add(prof::C_IDLE_OK, 1);
  skip9 = skip[0];
  skip7 = skip[1];
  return true;
}

bool Scheduler::both_idle() const {
  const CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  const CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  auto asleep = [](const CpuContext& c) { return c.halted && !(c.hot.irq_pending && !(c.hot.cpsr & 0x80)); };
  return asleep(a9) && asleep(a7) && !nds_.dma.any_running(Cpu::ARM9) && !nds_.dma.any_running(Cpu::ARM7) && nds_.gpu3d.idle();
}

// DS_PROFILE=1: what the slices are made of.
void Scheduler::count_slice(bool skipped, s64 slice) const {
  const CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  const CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  prof::add(prof::C_SLICES, 1);
  if (a9.halted) prof::add(prof::C_SLICES_A9_HALTED, 1);
  if (a7.halted) prof::add(prof::C_SLICES_A7_HALTED, 1);
  if (a9.halted && a7.halted) prof::add(prof::C_SLICES_BOTH_HALTED, 1);
  if (nds_.dma.any_running(Cpu::ARM9) || nds_.dma.any_running(Cpu::ARM7)) prof::add(prof::C_SLICES_DMA, 1);
  if (skipped) prof::add(prof::C_SLICES_SKIPPED, 1);

  // Cycle-weighted halt state: slice counts hide it, because the slices where
  // a CPU is awake are the ones the quantum keeps short.
  const u64 cyc = static_cast<u64>(slice);
  prof::add(prof::C_CYC_TOTAL, cyc);
  if (a9.halted && a7.halted) prof::add(prof::C_CYC_BOTH_HALTED, cyc);
  else if (a9.halted) prof::add(prof::C_CYC_A9_ONLY_HALTED, cyc);
  else if (a7.halted) prof::add(prof::C_CYC_A7_ONLY_HALTED, cyc);
  else prof::add(prof::C_CYC_NEITHER_HALTED, cyc);

  // Spin proxy for the awake CPUs.
  bool spin[2] = {false, false};
  const CpuContext* cpus[2] = {&a9, &a7};
  for (int i = 0; i < 2; ++i) {
    if (cpus[i]->halted) continue;
    const u32 pc = cpus[i]->hot.regs[15];
    for (u32 k = 0; k < 8; ++k) if (spin_ring_[i][k] == pc) { spin[i] = true; break; }
    spin_ring_[i][spin_pos_[i]++ & 7] = pc;
  }
  spin_now_[0] = spin[0]; spin_now_[1] = spin[1];
  // DS_DUMP_CODE=<hex addr>: one-shot dump of 16 guest words, for inspecting
  // a loop body the analyser rejected.
  static const char* dump_env = std::getenv("DS_DUMP_CODE");
  if (dump_env) {
    static bool done = false;
    if (!done) {
      const u32 base = static_cast<u32>(std::strtoul(dump_env, nullptr, 16));
      CpuContext& c = const_cast<CpuContext&>(nds_.cpu(Cpu::ARM9));
      const u32 at = c.hot.regs[15] - 8;   // wait until the CPU is actually in there
      if (at >= base && at < base + 64 && c.page_table.read_ptr(base)) {
        done = true;
        for (u32 k = 0; k < 16; ++k) {
          u32 w = 0;
          if (const u8* hp = c.page_table.read_ptr(base + k * 4)) std::memcpy(&w, hp, 4);
          std::fprintf(stderr, "[code] %08x  %08x\n", base + k * 4, w);
        }
      }
    }
  }
  // DS_SPIN_PCS=1: where the spin slices actually sit, to tell an idle poll
  // loop (a few addresses) from a merely hot inner loop (many).
  static const bool spin_pcs = std::getenv("DS_SPIN_PCS") != nullptr;
  if (spin_pcs) {
    static std::map<u32, u64> hist[2];
    static std::map<u32, u32> opc[2];
    static std::map<u32, const char*> why[2];
    spin_opcodes_ = &opc[0];
    spin_reject_ = &why[0];
    static bool reg = false;
    if (!reg) {
      reg = true;
      std::atexit([] {
        for (int i = 0; i < 2; ++i) {
          std::vector<std::pair<u64, u32>> v;
          u64 tot = 0;
          for (auto& kv : hist[i]) { v.push_back({kv.second, kv.first}); tot += kv.second; }
          if (!tot) continue;
          std::sort(v.rbegin(), v.rend());
          std::fprintf(stderr, "[spin] %s: %zu distinct pcs, %llu cycles\n", i ? "arm7" : "arm9",
                       v.size(), (unsigned long long)tot);
          for (size_t k = 0; k < v.size() && k < 12; ++k)
            std::fprintf(stderr, "[spin]   %08x  op %08x  %-12s %12llu %5.1f%%\n", v[k].second,
                         spin_opcodes_ ? spin_opcodes_[i][v[k].second] : 0u,
                         spin_reject_ ? spin_reject_[i][v[k].second] : "?",
                         (unsigned long long)v[k].first, 100.0 * static_cast<double>(v[k].first) / static_cast<double>(tot));
        }
      });
    }
    for (int i = 0; i < 2; ++i) if (spin[i]) {
      const u32 pc = cpus[i]->hot.regs[15];
      hist[i][pc] += cyc;
      const bool th = cpus[i]->thumb();
      const u32 at = pc - (th ? 4 : 8);   // regs[15] runs ahead of the executing instruction
      u32 w = 0;
      if (const u8* hp = cpus[i]->page_table.read_ptr(at)) std::memcpy(&w, hp, th ? 2 : 4);
      opc[i][pc] = w;
      CpuContext& dc = const_cast<CpuContext&>(*cpus[i]);
      why[i][pc] = cpu::in_idle_loop(dc) ? "ok" : cpu::idle_reject_name(cpu::idle_loop_last_reject());
    }
  }
  if (spin[0]) prof::add(prof::C_CYC_A9_SPIN, cyc);
  if (spin[1]) prof::add(prof::C_CYC_A7_SPIN, cyc);
  const bool idle9 = a9.halted || spin[0];
  const bool idle7 = a7.halted || spin[1];
  if (idle9 && idle7) {
    prof::add(prof::C_CYC_BOTH_SPIN_OR_HALTED, cyc);
    if (!(a9.halted && a7.halted)) prof::add(prof::C_CYC_ONE_SPIN_ONE_HALTED, cyc);
  }
}

// Every armed event at or before now_, in table order, repeated until none
// is left: a handler may schedule an event (its own, or one earlier in the
// table) at a time the CPUs have already overshot — a timer with a period
// shorter than a slice does — and a single pass would leave it armed in the
// past, where run_until(next_deadline()) can never reach it.
void Scheduler::fire_due() {
  while (now_ >= next_) {
    // One walk of the armed set, not one to fire and one to rescan: the pass
    // starts with next_ empty and folds in every event it steps over, while
    // schedule() folds in every event a handler arms (it keeps next_ current
    // against whatever minimum stands). Both leave next_ the true minimum.
    next_ = std::numeric_limits<u64>::max();
    next_id_ = EVENT_COUNT;
    // Ascending id order, i.e. table order, as when this walked the array.
    // `armed_` is re-read after every handler so an event the handler arms at
    // a higher id still fires in this pass, and one it arms at a lower id
    // waits for the next -- exactly what the array walk did.
    for (u32 m = armed_; m; ) {
      const u32 i = static_cast<u32>(__builtin_ctz(m));
      const u32 bit = 1u << i;
      if (at_[i] <= now_) {
        armed_ &= ~bit;
        firing_at_ = at_[i];
        fn_[i](nds_, param_[i]);   // may schedule: next_ is kept current by schedule()
        m = armed_ & ~((bit << 1) - 1);
      } else {
        if (at_[i] < next_) { next_ = at_[i]; next_id_ = i; }
        m &= ~bit;
      }
    }
  }
}

// One CPU's share of a slice: a running DMA goes first (the CPU is stalled),
// then the CPU runs; a DMA it starts preempts it and the loop hands the
// remaining budget to the DMA before the CPU continues.
void Scheduler::run_cpu(CpuContext& cpu, RunFn run) {
  const Cpu which = cpu.which;
  for (;;) {
    if (nds_.dma.any_running(which)) {
      { DS_PROF(DMA); in_dma_ = true; cpu.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(which, static_cast<u32>(cpu.hot.cycle_budget))); in_dma_ = false; }
      if (cpu.hot.cycle_budget <= 0 || nds_.dma.any_running(which) || a9_gx_stalled(cpu)) return;
    }
    {
      const auto t0 = std::chrono::steady_clock::now();
      run(cpu);
      if (prof::enabled) {
        const int ci = which == Cpu::ARM9 ? 0 : 1;
        const u64 el = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
        prof::add_ns(ci == 0 ? prof::CPU9 : prof::CPU7, el);
        prof::add(spin_now_[ci] ? (ci == 0 ? prof::C_NS_A9_SPIN : prof::C_NS_A7_SPIN)
                                : (ci == 0 ? prof::C_NS_A9_WORK : prof::C_NS_A7_WORK), el);
      }
    }
    if (!cpu.preempt_residual) return;
    cpu.hot.cycle_budget += cpu.preempt_residual;   // overshoot of the preempted instruction comes off the residual
    cpu.preempt_residual = 0;
    if (cpu.yielded) { cpu.yielded = false; return; }   // yield(): the rest of the slice goes to the other CPU
    if (cpu.hot.cycle_budget <= 0 || cpu.halted) return;
    if (a9_gx_stalled(cpu)) return;   // gx_fifo_full: sits out until the FIFO drains
  }
}

#if DSPERATE_JIT
// ---- native slice loop ----------------------------------------------------------
//
// run_until / run_cpu / jit::run cut at the points where translated code is
// entered, written as one straight-line sequence per slice with two resume
// points at the top (the in-order core mispredicts jump tables and indirect
// calls; the common path here is a handful of well-predicted branches). The
// order of operations is exactly the one of run_until below; the strict
// slice diff and the JIT-vs-JIT frame diff check it.

namespace {
enum { SL_BEGIN, SL_A9, SL_A7 };
}

SliceNext Scheduler::slice_next() {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  CpuContext* cpu;
  RunFn run;
  if (sl_.phase == SL_A9) { cpu = &a9; run = nds_.run_arm9; goto resume; }
  if (sl_.phase == SL_A7) { cpu = &a7; run = nds_.run_arm7; goto resume; }

begin:
  {
    if (sl_.until_frame ? nds_.frame_ready : now_ >= sl_.until) { sl_.phase = SL_BEGIN; return {nullptr, nullptr}; }
    u64 deadline = next_;
    if (deadline > sl_.until) deadline = sl_.until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    // With both CPUs asleep the quantum only paces the clock: run to the deadline.
    const bool all_idle = machine_idle(sl_.skip9, sl_.skip7);
    const bool idle = slice > quantum_ && all_idle;
    if (slice > quantum_ && !idle) slice = quantum_;
    if (slice > LOCKSTEP_QUANTUM && nds_.gpu3d.stalled()) slice = LOCKSTEP_QUANTUM;   // event-bound: poll the FIFO drain
    sl_.slice = slice;
    if (prof::enabled) count_slice(idle, slice);
    if (prof::enabled && (sl_.skip9 || sl_.skip7)) prof::add(prof::C_CYC_IDLE_SKIPPED, static_cast<u64>(slice));
    a9.hot.cycle_budget = static_cast<s32>(slice);
    running_ = &a9; running_start_budget_ = static_cast<s32>(slice); running_shift_ = 0;
    sl_.gx_stalled = nds_.gpu3d.stalled();
    sl_.phase = SL_A9; cpu = &a9; run = nds_.run_arm9;
    if (sl_.gx_stalled || sl_.skip9) goto a9_done;
  }
cpu_begin:   // run_cpu loop head
  {
    if (nds_.dma.any_running(cpu->which)) {
      { DS_PROF(DMA); in_dma_ = true; cpu->hot.cycle_budget -= static_cast<s32>(nds_.dma.run(cpu->which, static_cast<u32>(cpu->hot.cycle_budget))); in_dma_ = false; }
      if (cpu->hot.cycle_budget <= 0 || nds_.dma.any_running(cpu->which) || a9_gx_stalled(*cpu)) goto cpu_done;
    }
    if (prof::enabled) sl_.t0 = std::chrono::steady_clock::now();
    if (!cpu->jit) { run(*cpu); goto run_returned; }
    // jit::run up to the first entry
    if (cpu->hot.irq_pending) cpu->check_irq();
    if (cpu->halted) { cpu->hot.cycle_budget = -1; goto run_returned; }
    if (cpu->step_limit) { interp::run(*cpu); goto run_returned; }
    if (cpu->hot.cycle_budget > 0) return {cpu, jit::lookup(*cpu)};
    goto run_returned;
  }
resume:      // translated code left
  {
    if (cpu->halted) { cpu->budget_at_halt = cpu->hot.cycle_budget; cpu->hot.cycle_budget = -1; goto run_returned; }
    if (cpu->hot.irq_pending) cpu->check_irq();
    if (cpu->hot.cycle_budget > 0) return {cpu, jit::lookup(*cpu)};
  }
run_returned:
  {
    if (prof::enabled) {
      const int ci = cpu->which == Cpu::ARM9 ? 0 : 1;
      const u64 el = static_cast<u64>((std::chrono::steady_clock::now() - sl_.t0).count());
      prof::add_ns(ci == 0 ? prof::CPU9 : prof::CPU7, el);
      prof::add(spin_now_[ci] ? (ci == 0 ? prof::C_NS_A9_SPIN : prof::C_NS_A7_SPIN)
                              : (ci == 0 ? prof::C_NS_A9_WORK : prof::C_NS_A7_WORK), el);
    }
    if (cpu->preempt_residual) {
      cpu->hot.cycle_budget += cpu->preempt_residual;
      cpu->preempt_residual = 0;
      if (cpu->yielded) cpu->yielded = false;   // yield(): the rest of the slice goes to the other CPU
      else if (cpu->hot.cycle_budget > 0 && !cpu->halted && !a9_gx_stalled(*cpu)) goto cpu_begin;
    }
  }
cpu_done:
  if (cpu == &a7) goto a7_done;
a9_done:
  {
    s64 ran9 = (a9.halted || sl_.gx_stalled || sl_.skip9) ? sl_.slice : (sl_.slice - a9.hot.cycle_budget);
    if (ran9 <= 0) ran9 = 1;
    sl_.ran9 = ran9;
    running_ = nullptr;
    { DS_PROF(GX_RUN); nds_.gpu3d.run_to(now_ + static_cast<u64>(ran9)); }
    arm7_debt_ += ran9;
    sl_.budget7 = static_cast<s32>(arm7_debt_ / 2);
    if (sl_.budget7 <= 0) goto slice_end;
    a7.hot.cycle_budget = sl_.budget7;
    running_ = &a7; running_start_budget_ = sl_.budget7; running_shift_ = 1;
    sl_.phase = SL_A7; cpu = &a7; run = nds_.run_arm7;
    if (sl_.skip7) goto a7_done;
    goto cpu_begin;
  }
a7_done:
  {
    const s64 consumed7 = (a7.halted || sl_.skip7) ? sl_.budget7 : (sl_.budget7 - a7.hot.cycle_budget);
    arm7_debt_ -= consumed7 * 2;
  }
slice_end:
  {
    running_ = nullptr;
    now_ += static_cast<u64>(sl_.ran9);
    if (debug_slices_) std::fprintf(stderr, "[slice] now %llu ran9 %lld a9pc %08x b7 %d a7left %d a7pc %08x\n", (unsigned long long)now_, (long long)sl_.ran9, a9.hot.regs[15], sl_.budget7, a7.hot.cycle_budget, a7.hot.regs[15]);
    fire_due();
    goto begin;
  }
}

extern "C" SliceNext ds_slice_next(void* scheduler) { return static_cast<Scheduler*>(scheduler)->slice_next(); }

u64 Scheduler::run_until_native(u64 until, bool until_frame) {
  const u64 start = now_;
  fire_due();   // anything already due (see fire_due): the loop only fires at slice ends
  sl_.until = until;
  sl_.until_frame = until_frame;
  sl_.phase = SL_BEGIN;
  jit::run_loop(this);
  return now_ - start;
}

#endif // DSPERATE_JIT

// The frame flag is set by the line-0 scanline handler, i.e. from the fire_due
// at a slice end, so testing it at the next slice start stops at exactly the
// point `while (!frame_ready) run_until(next_deadline())` stopped at.
bool Scheduler::done(u64 until, bool until_frame) const {
  return until_frame ? nds_.frame_ready : now_ >= until;
}

u64 Scheduler::run_until(u64 until) { return run_until_impl(until, false); }

u64 Scheduler::run_until_frame() { return run_until_impl(~u64{0}, true); }

u64 Scheduler::run_until_impl(u64 until, bool until_frame) {
#if DSPERATE_JIT
  if (jit::has_runtime()) return run_until_native(until, until_frame);
#endif
  const u64 start = now_;
  fire_due();   // anything already due (see fire_due): the loop only fires at slice ends
  while (!done(until, until_frame)) {
    u64 deadline = next_deadline();
    if (deadline > until) deadline = until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    bool skip9 = false, skip7 = false;
    const bool all_idle = machine_idle(skip9, skip7);
    const bool idle = slice > quantum_ && all_idle;
    if (slice > quantum_ && !idle) slice = quantum_;
    if (slice > LOCKSTEP_QUANTUM && nds_.gpu3d.stalled()) slice = LOCKSTEP_QUANTUM;   // event-bound: poll the FIFO drain
    if (prof::enabled) count_slice(idle, slice);
    if (prof::enabled && (skip9 || skip7)) prof::add(prof::C_CYC_IDLE_SKIPPED, static_cast<u64>(slice));

    // ARM9 gets the whole slice; ARM7 then catches up at half clock.
    CpuContext& a9 = nds_.cpu(Cpu::ARM9);
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    a9.hot.cycle_budget = static_cast<s32>(slice);
    running_ = &a9; running_start_budget_ = static_cast<s32>(slice); running_shift_ = 0;
    // While the GX FIFO is full the ARM9 (and its DMA) sit out the slice;
    // the geometry engine keeps draining behind it.
    const bool gx_stalled = nds_.gpu3d.stalled();
    if (!gx_stalled && !skip9) run_cpu(a9, nds_.run_arm9);
    // A halted CPU consumes exactly the slice; a running one may overshoot,
    // and the overshoot is real time (it carries into the next slice).
    s64 ran9 = (a9.halted || gx_stalled || skip9) ? slice : (slice - a9.hot.cycle_budget);
    if (ran9 <= 0) ran9 = 1;
    running_ = nullptr;
    { DS_PROF(GX_RUN); nds_.gpu3d.run_to(now_ + static_cast<u64>(ran9)); }

    // The ARM7 runs at half clock and must cover the same span of time. Its
    // overshoot and the odd ARM9 cycle are carried in arm7_debt_, the way
    // melonDS carries absolute timestamps, so it neither gains nor loses time.
    arm7_debt_ += ran9;
    const s32 budget7 = static_cast<s32>(arm7_debt_ / 2);
    if (budget7 > 0) {
      a7.hot.cycle_budget = budget7;
      running_ = &a7; running_start_budget_ = budget7; running_shift_ = 1;
      if (!skip7) run_cpu(a7, nds_.run_arm7);
      const s64 consumed7 = (a7.halted || skip7) ? budget7 : (budget7 - a7.hot.cycle_budget);
      arm7_debt_ -= consumed7 * 2;
    }
    running_ = nullptr;

    now_ += static_cast<u64>(ran9);
    // DS_DEBUG_SLICES=1: one line per slice (engine lockstep debugging).
    if (debug_slices_) std::fprintf(stderr, "[slice] now %llu ran9 %lld a9pc %08x b7 %d a7left %d a7pc %08x\n", (unsigned long long)now_, (long long)ran9, a9.hot.regs[15], budget7, a7.hot.cycle_budget, a7.hot.regs[15]);
    fire_due();
  }
  return now_ - start;
}

} // namespace ds
