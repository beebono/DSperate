// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/sched/scheduler.h"
#include "core/nds.h"
#include "core/profile.h"
#include "core/cpu/interp/interp.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace ds {

Scheduler::Scheduler(NDS& nds) : nds_(nds), now_(0) {
  // Read once: a function-local static costs an acquire load per use.
  if (const char* q = std::getenv("DS_QUANTUM")) quantum_ = std::atoll(q);
  debug_slices_ = std::getenv("DS_DEBUG_SLICES") != nullptr;
  reset();
}

void Scheduler::reset() {
  now_ = 0;
  arm7_debt_ = 0;
  for (auto& e : events_) e = Event{0, nullptr, 0, false};
  next_ = std::numeric_limits<u64>::max();
}

// `next_` caches the earliest armed deadline so the per-slice loop scans the
// table only when an event is actually due (or after a cancel).
void Scheduler::schedule(EventId id, u64 at, EventFn fn, u32 param) {
  Event& e = events_[static_cast<size_t>(id)];
  const bool was_next = e.armed && e.at == next_;
  e = Event{at, fn, param, true};
  if (at < next_) next_ = at;
  else if (was_next && at > next_) next_ = scan_deadline();
}

void Scheduler::cancel(EventId id) {
  Event& e = events_[static_cast<size_t>(id)];
  if (!e.armed) return;
  e.armed = false;
  if (e.at == next_) next_ = scan_deadline();
}

u64 Scheduler::scan_deadline() const {
  u64 best = std::numeric_limits<u64>::max();
  for (const auto& e : events_)
    if (e.armed && e.at < best) best = e.at;
  return best;
}

void Scheduler::fire_due() {
  if (now_ < next_) return;
  for (auto& e : events_) {
    if (e.armed && e.at <= now_) {
      e.armed = false;
      e.fn(nds_, e.param);      // may schedule: next_ is kept current by schedule()
    }
  }
  next_ = scan_deadline();
}

// One CPU's share of a slice: a running DMA goes first (the CPU is stalled),
// then the CPU runs; a DMA it starts preempts it and the loop hands the
// remaining budget to the DMA before the CPU continues.
void Scheduler::run_cpu(CpuContext& cpu, RunFn run) {
  const Cpu which = cpu.which;
  for (;;) {
    if (nds_.dma.any_running(which)) {
      { DS_PROF(DMA); cpu.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(which, static_cast<u32>(cpu.hot.cycle_budget))); }
      if (cpu.hot.cycle_budget <= 0 || nds_.dma.any_running(which)) return;
    }
    { prof::Scope sc(which == Cpu::ARM9 ? prof::CPU9 : prof::CPU7); run(cpu); }
    if (!cpu.preempt_residual) return;
    cpu.hot.cycle_budget += cpu.preempt_residual;   // overshoot of the preempted instruction comes off the residual
    cpu.preempt_residual = 0;
    if (cpu.hot.cycle_budget <= 0 || cpu.halted) return;
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
    if (now_ >= sl_.until) { sl_.phase = SL_BEGIN; return {nullptr, nullptr}; }
    u64 deadline = next_;
    if (deadline > sl_.until) deadline = sl_.until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    if (slice > quantum_) slice = quantum_;
    sl_.slice = slice;
    a9.hot.cycle_budget = static_cast<s32>(slice);
    running_ = &a9; running_start_budget_ = static_cast<s32>(slice); running_shift_ = 0;
    sl_.gx_stalled = nds_.gpu3d.stalled();
    sl_.phase = SL_A9; cpu = &a9; run = nds_.run_arm9;
    if (sl_.gx_stalled) goto a9_done;
  }
cpu_begin:   // run_cpu loop head
  {
    if (nds_.dma.any_running(cpu->which)) {
      { DS_PROF(DMA); cpu->hot.cycle_budget -= static_cast<s32>(nds_.dma.run(cpu->which, static_cast<u32>(cpu->hot.cycle_budget))); }
      if (cpu->hot.cycle_budget <= 0 || nds_.dma.any_running(cpu->which)) goto cpu_done;
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
    if (prof::enabled) prof::ns[cpu->which == Cpu::ARM9 ? prof::CPU9 : prof::CPU7] += static_cast<u64>((std::chrono::steady_clock::now() - sl_.t0).count());
    if (cpu->preempt_residual) {
      cpu->hot.cycle_budget += cpu->preempt_residual;
      cpu->preempt_residual = 0;
      if (cpu->hot.cycle_budget > 0 && !cpu->halted) goto cpu_begin;
    }
  }
cpu_done:
  if (cpu == &a7) goto a7_done;
a9_done:
  {
    s64 ran9 = (a9.halted || sl_.gx_stalled) ? sl_.slice : (sl_.slice - a9.hot.cycle_budget);
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
    goto cpu_begin;
  }
a7_done:
  {
    const s64 consumed7 = a7.halted ? sl_.budget7 : (sl_.budget7 - a7.hot.cycle_budget);
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

u64 Scheduler::run_until_native(u64 until) {
  const u64 start = now_;
  sl_.until = until;
  sl_.phase = SL_BEGIN;
  jit::run_loop(this);
  return now_ - start;
}

#endif // DSPERATE_JIT

u64 Scheduler::run_until(u64 until) {
#if DSPERATE_JIT
  if (jit::has_runtime()) return run_until_native(until);
#endif
  const u64 start = now_;
  while (now_ < until) {
    u64 deadline = next_deadline();
    if (deadline > until) deadline = until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    // DS_QUANTUM=<cycles>: measurement knob only; anything but 128 breaks lockstep with melonDS.
    if (slice > quantum_) slice = quantum_;

    // ARM9 gets the whole slice; ARM7 then catches up at half clock.
    CpuContext& a9 = nds_.cpu(Cpu::ARM9);
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    a9.hot.cycle_budget = static_cast<s32>(slice);
    running_ = &a9; running_start_budget_ = static_cast<s32>(slice); running_shift_ = 0;
    // While the GX FIFO is full the ARM9 (and its DMA) sit out the slice;
    // the geometry engine keeps draining behind it.
    const bool gx_stalled = nds_.gpu3d.stalled();
    if (!gx_stalled) run_cpu(a9, nds_.run_arm9);
    // A halted CPU consumes exactly the slice; a running one may overshoot,
    // and the overshoot is real time (it carries into the next slice).
    s64 ran9 = (a9.halted || gx_stalled) ? slice : (slice - a9.hot.cycle_budget);
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
      run_cpu(a7, nds_.run_arm7);
      const s64 consumed7 = a7.halted ? budget7 : (budget7 - a7.hot.cycle_budget);
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
