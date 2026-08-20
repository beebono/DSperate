// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds {

struct NDS;

// Event scheduler and the CPU interleave.
//
// Time is a u64 count of ARM9 cycles. Each CPU runs from a *downward* budget
// (CpuContext::hot.cycle_budget) handed out by the scheduler: the budget is the
// distance to the next pending event, and the engine (interpreter or JIT) runs
// until the sign bit sets. That single check is the only scheduling test in the
// hot path (docs/ARCHITECTURE.md §4).
//
// The ARM7 runs at half clock; its budget is issued in ARM7 cycles and the
// scheduler converts.

enum class EventId : u8 {
  HBlank, VBlank_Scanline, Timer0, Timer1, Timer2, Timer3,
  Timer7_0, Timer7_1, Timer7_2, Timer7_3,
  Dma, Spu, Rtc, Cart, Gx3D, Count
};

using EventFn = void (*)(NDS& nds, u32 param);

class Scheduler {
public:
  explicit Scheduler(NDS& nds);

  void reset();

  void schedule(EventId id, u64 at, EventFn fn, u32 param = 0);
  void cancel(EventId id);

  u64 now() const { return now_; }
  u64 next_deadline() const;

  // Runs ARM9 then ARM7 up to the next event, fires due events, repeats until
  // `until` is reached. Returns the number of cycles advanced.
  u64 run_until(u64 until);

private:
  struct Event {
    u64     at;
    EventFn fn;
    u32     param;
    bool    armed;
  };
  NDS& nds_;
  u64  now_;
  std::array<Event, static_cast<size_t>(EventId::Count)> events_;
  void fire_due();
};

} // namespace ds
