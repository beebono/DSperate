// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/sched/scheduler.h"
#include "core/nds.h"

#include <limits>

namespace ds {

Scheduler::Scheduler(NDS& nds) : nds_(nds), now_(0) { reset(); }

void Scheduler::reset() {
  now_ = 0;
  arm7_debt_ = 0;
  for (auto& e : events_) e = Event{0, nullptr, 0, false};
}

void Scheduler::schedule(EventId id, u64 at, EventFn fn, u32 param) {
  events_[static_cast<size_t>(id)] = Event{at, fn, param, true};
}

void Scheduler::cancel(EventId id) { events_[static_cast<size_t>(id)].armed = false; }

u64 Scheduler::next_deadline() const {
  u64 best = std::numeric_limits<u64>::max();
  for (const auto& e : events_)
    if (e.armed && e.at < best) best = e.at;
  return best;
}

void Scheduler::fire_due() {
  for (auto& e : events_) {
    if (e.armed && e.at <= now_) {
      e.armed = false;
      e.fn(nds_, e.param);
    }
  }
}

u64 Scheduler::run_until(u64 until) {
  const u64 start = now_;
  while (now_ < until) {
    u64 deadline = next_deadline();
    if (deadline > until) deadline = until;
    s64 slice = static_cast<s64>(deadline - now_);
    if (slice <= 0) slice = 1;
    if (slice > INTERLEAVE_QUANTUM) slice = INTERLEAVE_QUANTUM;

    // ARM9 gets the whole slice; ARM7 then catches up at half clock.
    CpuContext& a9 = nds_.cpu(Cpu::ARM9);
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    a9.hot.cycle_budget = static_cast<s32>(slice);
    running_ = &a9; running_start_budget_ = static_cast<s32>(slice); running_shift_ = 0;
    if (nds_.dma.any_running(Cpu::ARM9)) {
      a9.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(Cpu::ARM9, static_cast<u32>(slice)));
      if (a9.hot.cycle_budget > 0 && !nds_.dma.any_running(Cpu::ARM9)) nds_.run_arm9(a9);
    } else {
      nds_.run_arm9(a9);
    }
    // A halted CPU consumes exactly the slice; a running one may overshoot,
    // and the overshoot is real time (it carries into the next slice).
    s64 ran9 = a9.halted ? slice : (slice - a9.hot.cycle_budget);
    if (ran9 <= 0) ran9 = 1;

    // The ARM7 runs at half clock and must cover the same span of time. Its
    // overshoot and the odd ARM9 cycle are carried in arm7_debt_, the way
    // melonDS carries absolute timestamps, so it neither gains nor loses time.
    arm7_debt_ += ran9;
    const s32 budget7 = static_cast<s32>(arm7_debt_ / 2);
    if (budget7 > 0) {
      a7.hot.cycle_budget = budget7;
      running_ = &a7; running_start_budget_ = budget7; running_shift_ = 1;
      if (nds_.dma.any_running(Cpu::ARM7)) {
        a7.hot.cycle_budget -= static_cast<s32>(nds_.dma.run(Cpu::ARM7, static_cast<u32>(budget7)));
        if (a7.hot.cycle_budget > 0 && !nds_.dma.any_running(Cpu::ARM7)) nds_.run_arm7(a7);
      } else {
        nds_.run_arm7(a7);
      }
      const s64 consumed7 = a7.halted ? budget7 : (budget7 - a7.hot.cycle_budget);
      arm7_debt_ -= consumed7 * 2;
    }
    running_ = nullptr;

    now_ += static_cast<u64>(ran9);
    fire_due();
  }
  return now_ - start;
}

} // namespace ds
