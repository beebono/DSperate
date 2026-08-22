// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"

#include <array>
#include <chrono>

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
  Dma, Spu, Spi, Rtc, Cart, Gx3D, DisplayFifo, Div, Sqrt, Count
};

// CPU interleave quantum in ARM9 cycles. 128 matches melonDS's 64 system cycles,
// which keeps IPC handshakes in the same order for trace comparison.
constexpr u32 INTERLEAVE_QUANTUM = 128;

using EventFn = void (*)(NDS& nds, u32 param);

// What the native slice loop runs next: the context to enter and the native
// entry (returned in x0/x1); ctx == nullptr ends the run.
struct SliceNext { CpuContext* ctx; const void* native; };

class Scheduler {
public:
  explicit Scheduler(NDS& nds);

  void reset();

  void schedule(EventId id, u64 at, EventFn fn, u32 param = 0);
  void cancel(EventId id);

  // Current time. While a CPU is executing its slice this includes the cycles
  // it has consumed so far, so events scheduled from inside an instruction
  // (SPI, cart, timers) are stamped relative to that CPU's own position.
  u64 now() const {
    if (!running_) return now_;
    return now_ + (static_cast<u64>(running_start_budget_ - running_->hot.cycle_budget - running_->preempt_residual) << running_shift_);
  }
  const CpuContext* running() const { return running_; }

  // Called when an immediate DMA starts on `cpu`: if that CPU is the one
  // executing, it leaves its run loop after the current instruction and the
  // DMA takes over the rest of its slice, as the bus stall does on hardware.
  void preempt(CpuContext& cpu) {
    if (running_ != &cpu || cpu.hot.cycle_budget <= 0) return;
    cpu.preempt_residual += cpu.hot.cycle_budget;
    cpu.hot.cycle_budget = 0;
  }
  u64 next_deadline() const { return next_; }

  // Runs ARM9 then ARM7 up to the next event, fires due events, repeats until
  // `until` is reached. Returns the number of cycles advanced.
  u64 run_until(u64 until);

  // The same, as a state machine for the recompiler's native slice loop
  // (docs/ARCHITECTURE.md §4): every call runs the scheduler up to the next
  // entry into translated code and returns it; the loop enters it and calls
  // again when the code leaves. Interpreted CPUs and DMA run inside.
  SliceNext slice_next();

private:
  struct SliceState {
    u64 until = 0;
    int phase = 0, sub = 0;
    s64 slice = 0, ran9 = 0;
    s32 budget7 = 0;
    bool gx_stalled = false;
    CpuContext* cpu = nullptr;
    std::chrono::steady_clock::time_point t0;
  } sl_;
  u64 run_until_native(u64 until);
  u64 scan_deadline() const;
  u64 next_ = ~u64{0};   // earliest armed deadline (cached)
  s64  quantum_ = INTERLEAVE_QUANTUM;   // DS_QUANTUM override (measurement only)
  bool debug_slices_ = false;           // DS_DEBUG_SLICES
  struct Event {
    u64     at;
    EventFn fn;
    u32     param;
    bool    armed;
  };
  NDS& nds_;
  u64  now_;
  const CpuContext* running_ = nullptr;
  s32  running_start_budget_ = 0;
  u32  running_shift_ = 0;           // 0 for ARM9 cycles, 1 for ARM7 (half clock)
  s64  arm7_debt_ = 0;               // ARM9 cycles the ARM7 still has to cover (carries overshoot and odd cycles)
  std::array<Event, static_cast<size_t>(EventId::Count)> events_;
  void fire_due();
  void run_cpu(CpuContext& cpu, RunFn run);
};

} // namespace ds
