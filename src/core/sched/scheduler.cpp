// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/sched/scheduler.h"
#include "core/nds.h"

#include <limits>

namespace ds {

Scheduler::Scheduler(NDS& nds) : nds_(nds), now_(0) { reset(); }

void Scheduler::reset() {
  now_ = 0;
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

    // ARM9 gets the whole slice; ARM7 then catches up at half clock.
    CpuContext& a9 = nds_.cpu(Cpu::ARM9);
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    a9.hot.cycle_budget = static_cast<s32>(slice);
    nds_.run_arm9(a9);
    s64 ran9 = slice - a9.hot.cycle_budget;      // budget went negative => overshoot
    a7.hot.cycle_budget = static_cast<s32>(ran9 / 2);
    nds_.run_arm7(a7);

    now_ += static_cast<u64>(ran9);
    fire_due();
  }
  return now_ - start;
}

} // namespace ds
