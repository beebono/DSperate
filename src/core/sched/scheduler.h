// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <map>
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
// hot path.
//
// The ARM7 runs at half clock; its budget is issued in ARM7 cycles and the
// scheduler converts.

enum class EventId : u8 {
  HBlank, VBlank_Scanline, Timer0, Timer1, Timer2, Timer3,
  Timer7_0, Timer7_1, Timer7_2, Timer7_3,
  Dma, Spu, Spi, Rtc, Cart, Gx3D, DisplayFifo, Div, Sqrt, LcdIrq, Wifi,
  // DSi only: melonDS's periodic RTC clock (32768 Hz) and camera IRQ
  // (~15 fps) events, kept as slice boundaries so the CPU interleave
  // matches the oracle's (a DSiWare loader measures one CPU against the
  // other); neither drives any state yet (Io::grid_rtc_event / grid_cam_event).
  RtcClock, CamIrq, SdMmc, Count
};

// CPU interleave quantum in ARM9 cycles: the most one CPU runs before the
// other catches up. LOCKSTEP_QUANTUM (melonDS's 64 system cycles) keeps IPC
// handshakes in the same order as melonDS for trace and frame comparison;
// 0 means event-bound — each CPU runs to the next scheduled event, as
// DraStic does — which is 5-10 % faster and what the frontends run with.
constexpr u32 LOCKSTEP_QUANTUM = 128;
constexpr u32 INTERLEAVE_QUANTUM = LOCKSTEP_QUANTUM;   // the core's default (the verification harness)
// Event-bound is still capped: the SDK's IPCSYNC boot handshake counts down
// with a retry timeout on the ARM7 side, and lets one CPU run more than
// ~2.5 k ARM9 cycles unanswered and it never completes (SM64DS: 2048 boots,
// 2560 does not). The cap used to be implicit -- the SPU mixed one sample per
// 2048-cycle event, so no slice was ever longer -- and became explicit when
// the SPU started mixing in batches. Same slice count as before, so no cost.
constexpr u32 EVENT_BOUND_QUANTUM = 2048;

using EventFn = void (*)(NDS& nds, u32 param);

// What the native slice loop runs next: the context to enter and the native
// entry (returned in x0/x1); ctx == nullptr ends the run.
struct SliceNext { CpuContext* ctx; const void* native; };

class Scheduler {
public:
  explicit Scheduler(NDS& nds);

  void reset();

  // Interleave quantum in ARM9 cycles; 0 = event-bound. DS_QUANTUM in the
  // environment overrides whatever the frontend sets.
  void set_quantum(s64 q);
  void set_clock9_shift(u32 timing_shift) { shift9_ = timing_shift - 1; arm9_carry_ = 0; slice_margin_ = shift9_ ? 16 : 0; cut_on_schedule_ = shift9_ != 0; update_soft_mask(); }   // Timing::clock9_shift (1 or 2)
  u32  clock9_shift() const { return shift9_; }

  void schedule(EventId id, u64 at, EventFn fn, u32 param = 0);
  void cancel(EventId id);

  // Current time. While a CPU is executing its slice this includes the cycles
  // it has consumed so far, so events scheduled from inside an instruction
  // (SPI, cart, timers) are stamped relative to that CPU's own position.
  u64 now() const {
    if (!running_) return now_;
    // A DMA's progress counts too (dma_progress): melonDS's DMA loop advances
    // the CPU timestamp per unit, so a cart word read by a DMA schedules the
    // next from the DMA's own position. DSi only (the DS keeps its lazy model).
    const u64 c = static_cast<u64>(running_start_budget_ - running_->hot.cycle_budget - running_->preempt_residual) + dma_used_;
    // DSi ARM9 (134 MHz): its core time, carry from the last slice included,
    // floored to whole ARM7 cycles as melonDS's ARM9Timestamp >> 2 is.
    if (running_rshift_) return running_base_ + (((c + running_carry_) >> 2) << 1);
    return running_base_ + (c << running_shift_);
  }
  const CpuContext* running() const { return running_; }
  bool idle_skip_enabled() const { return idle_skip_ != 0; }

  // Called when an immediate DMA starts on `cpu`: if that CPU is the one
  // executing, it leaves its run loop after the current instruction and the
  // DMA takes over the rest of its slice, as the bus stall does on hardware.
  void preempt(CpuContext& cpu) {
    if (running_ != &cpu || cpu.hot.cycle_budget <= 0) return;
    cpu.preempt_residual += cpu.hot.cycle_budget;
    cpu.hot.cycle_budget = 0;
  }
  // Called when `cpu` writes something the other CPU is waiting on with a
  // tight timeout (IPCSYNC: the SDK's boot handshake sends a value and
  // restarts unless it is echoed within ~1600 cycles, which hardware does in
  // a few). The slice ends after the current instruction, the unrun budget
  // is dropped from the clock like a preempt's, and the other CPU runs next
  // instead of this one resuming -- so a slice longer than that timeout no
  // longer lets the writer time out before the reader has run at all.
  // Only above the lockstep quantum: at 64 system cycles the reader runs
  // within the timeout anyway, and the yield moves the writer's remaining
  // work a slice later than melonDS's catch-up order, which the trace
  // harness sees (a DSiWare loader measures the ARM7 against the ARM9).
  void yield(CpuContext& cpu) {
    if (quantum_ <= LOCKSTEP_QUANTUM) return;
    if (running_ != &cpu || cpu.hot.cycle_budget <= 0) return;
    cpu.yielded = true;
    cpu.preempt_residual += cpu.hot.cycle_budget;
    cpu.hot.cycle_budget = 0;
  }
  u64 next_deadline() const { return next_; }
  // The geometry FIFO just filled under the ARM9: in event-bound mode its
  // slice ends here (as the bus stall would), and it sits out until the
  // FIFO drains, re-checking every LOCKSTEP_QUANTUM cycles. In lockstep the
  // stall queue absorbs the rest of the 128-cycle slice, as before.
  void gx_fifo_full();
  bool in_dma() const { return in_dma_; }
  void dma_progress(u32 used) { if (shift9_) dma_used_ = used; }   // DSi only
  // Event-bound mode: a GX-stalled ARM9 sits out (lockstep keeps queueing, as melonDS's timing assumes).
  bool a9_gx_stalled(const CpuContext& cpu) const;

  // Nominal time of the event whose handler is running. Events fire at slice
  // ends, up to a CPU overshoot after their deadline; a periodic handler must
  // reschedule from this, not from now(), or the lateness accumulates into a
  // slow clock (measured: 0.04 % on the scanline and SPU events).
  u64 event_time() const { return firing_at_; }
  // The ARM7's own clock at a slice end: the slice end plus the cycles the
  // ARM7's last instruction ran past it. melonDS bases a one-shot event
  // scheduled from an event handler on this (its ARM7Timestamp, the
  // current CPU during RunSystem), so a chain of cart word events drifts
  // by the ARM7's overshoot each; DSi handlers that mirror it use this.
  // arm7_debt_ is what the ARM7 still has to cover to reach now_, so its own
  // clock is now_ - debt (negative debt = it ran past the slice end).
  u64 event_base7() const { return static_cast<u64>(static_cast<s64>(now_) - arm7_debt_); }

  // Runs ARM9 then ARM7 up to the next event, fires due events, repeats until
  // `until` is reached. Returns the number of cycles advanced.
  u64 run_until(u64 until);

  // The same, but bounded by the GPU's frame flag instead of a deadline: the
  // slice loop is entered once per frame rather than once per event, which is
  // what `while (!frame_ready) run_until(next_deadline())` used to cost
  // (~2,100 entries per frame on SM64DS, each with the native loop's register
  // save/restore and a fire_due that had nothing to fire).
  u64 run_until_frame();
  // Both bounds: stops at the frame flag or at `until`, whichever first. For
  // a frontend that spreads a frame's emulation across its wall-clock period
  // (NDS::run_frame_slice); the extra slice ends change the CPU interleave,
  // so a run paced this way is not the run run_frame() produces.
  u64 run_until_or_frame(u64 until);

  // The same, as a state machine for the recompiler's native slice loop:
  // every call runs the scheduler up to the next entry into translated
  // code and returns it; the loop enters it and calls again when the
  // code leaves. Interpreted CPUs and DMA run inside.
  SliceNext slice_next();

  // Save states (core/state/state.h). The handlers are function pointers,
  // so only the deadlines travel: each subsystem re-binds its own events
  // with rebind() while loading, and after_load() checks none is missing.
  template <class S> void sync_state(S& s);
  void rebind(EventId id, EventFn fn) { const u32 i = static_cast<u32>(id); if (armed_ & (1u << i)) fn_[i] = fn; }
  bool after_load();
  bool at_slice_boundary() const { return running_ == nullptr && !in_dma_; }


private:
  struct SliceState {
    u64 until = 0;
    bool until_frame = false;   // stop on NDS::frame_ready, not on `until`
    int phase = 0, sub = 0;
    s64 slice = 0, ran9 = 0;
    s32 budget7 = 0;
    s32 budget9 = 0;          // the ARM9's core-cycle budget for the slice (slice << shift9_)
    bool gx_stalled = false;
    bool skip9 = false, skip7 = false;   // proven idle loop: do not execute this slice
    CpuContext* cpu = nullptr;
    std::chrono::steady_clock::time_point t0;
  } sl_;
  u64 run_until_native(u64 until, bool until_frame);
  u64 run_until_impl(u64 until, bool until_frame);
  void rescan();
  bool done(u64 until, bool until_frame) const;
  u64 next_ = ~u64{0};   // earliest armed deadline (cached)
  u64 firing_at_ = 0;    // deadline of the event being fired
  s64  quantum_ = INTERLEAVE_QUANTUM;
  bool quantum_forced_ = false;         // DS_QUANTUM given
  bool in_dma_ = false;                 // inside Dma::run (a preempt there would corrupt the DMA's budget)
  u32  dma_used_ = 0;                   // budget units the current Dma::run has consumed so far (see now())
  // DSi: melonDS gives an ARM9 DMA its own iteration -- the ARM9 phase ends
  // when the DMA stops (or the budget runs out), the ARM7 catches up to that
  // point, and a DMA the ARM9 starts runs in the next iteration. Set when an
  // ARM9 phase ended that way; its length is then the cycles consumed, even
  // if the ARM9 is halted.
  bool a9_dma_iter_ = false;
  bool debug_slices_ = false;           // DS_DEBUG_SLICES
  // Idle-loop skip mode (DS_IDLE_SKIP): 0 off; 1 (default) only while the ARM9
  // is in a poll loop on GXSTAT with a swap pending -- the one shape measured
  // to pay (Dragon Ball Origins); 2 = "all": any proven poll loop, which costs
  // 7-10 % on scenes that never spin and moved Etrian Odyssey's frames.
  u8 idle_skip_ = 1;
  // The event table is split by field and gated by a bitmask: firing scans
  // only the armed events (typically six to eight of the twenty) and touches
  // three cache lines of deadlines instead of eight of interleaved records.
  static constexpr u32 EVENT_COUNT = static_cast<u32>(EventId::Count);
  std::array<u64, EVENT_COUNT>     at_{};
  std::array<EventFn, EVENT_COUNT> fn_{};
  std::array<u32, EVENT_COUNT>     param_{};
  u32 armed_ = 0;                 // bit i = events_[i] is armed
  u32 next_id_ = EVENT_COUNT;     // which event `next_` belongs to (EVENT_COUNT: none)
  NDS& nds_;
  u64  now_;
  const CpuContext* running_ = nullptr;
  s32  running_start_budget_ = 0;
  u32  running_shift_ = 0;           // 0 for ARM9 cycles, 1 for ARM7 (half clock)
  u32  running_rshift_ = 0;          // DSi: the ARM9 at twice the DS clock runs 2 core cycles per tick (shift9_)
  // Where the running CPU's clock starts this phase. The ARM9's is the slice
  // start. The ARM7's is the slice start on a DS (its overshoot into the
  // slice is not visible to now(), which the scene hashes are gated on); on
  // a DSi it is the ARM7's true position, slice end minus its remaining
  // debt, as melonDS's ARM7Timestamp is.
  u64  running_base_ = 0;
  u64  running_carry_ = 0;   // the ARM9's arm9_carry_ at the slice start (DSi)
  s64  arm7_debt_ = 0;               // ARM9 cycles the ARM7 still have to cover (carries overshoot and odd cycles)
  // DSi ARM9 clock: 0 on a DS (a tick is an ARM9 cycle), 1 at 134 MHz (a
  // tick is two). The ARM9 is budgeted in core cycles and its consumption
  // converted back to ticks here, the odd cycle carried in arm9_carry_.
  u32  shift9_ = 0;
  s64  arm9_carry_ = 0;
  // A CPU's boot stall (CpuContext::boot_stall) comes off its budget before it runs.
  void defer_preempt_cost(CpuContext& cpu);
  static void take_stall(CpuContext& cpu) {
    if (cpu.boot_stall <= 0) return;
    const s32 take = cpu.boot_stall < cpu.hot.cycle_budget ? cpu.boot_stall : cpu.hot.cycle_budget;
    cpu.boot_stall -= take; cpu.hot.cycle_budget -= take;
  }
  // ARM9 core cycles consumed -> scheduler ticks, the remainder carried.
  // melonDS keeps system time in ARM7 cycles and floors the ARM9's core
  // time to it: an overshoot under one ARM7 cycle (4 core cycles at
  // 134 MHz) does not move the next slice end, and the core cycles the ARM9
  // is already past the boundary (arm9_carry_, 0..3) come off its next
  // budget. Ticks are half ARM7 cycles, hence the extra bit.
  s32 budget9_for(s64 slice) const { return static_cast<s32>((slice << shift9_) - arm9_carry_); }
  s64 ticks9(s64 consumed9) {
    if (!shift9_) return consumed9;
    arm9_carry_ += consumed9;
    const s64 sys = arm9_carry_ >> (shift9_ + 1);
    arm9_carry_ -= sys << (shift9_ + 1);
    return sys << 1;
  }
  // melonDS runs a slice up to kIterationCycleMargin (8 ARM7 cycles) past
  // its quantum to land on an event rather than split off a tiny slice.
  // 0 on a DS (the DS interleave is gated by the scene hashes), 16 on a DSi.
  s64  slice_margin_ = 0;
  // melonDS: an event scheduled by the running ARM9 earlier than its target
  // shortens the target to it (NDS::ScheduleEvent), so the ARM7 catches up
  // only to that point and the event fires before the ARM9 continues. DSi
  // only (DS interleave gated by the scene hashes).
  bool cut_on_schedule_ = false;
  // melonDS steps its timers at slice ends (RunTimers) rather than as
  // scheduled events, so a timer overflow never shortens a slice and its IRQ
  // lands at the next boundary. "Soft" events reproduce that: fired when
  // due at a slice end, never a deadline. DSi at the lockstep quantum only:
  // an event-bound slice could hold a timer IRQ for thousands of cycles.
  u32  soft_mask_ = 0;
  u64  next_soft_ = ~u64{0};   // earliest armed soft event: fire_due scans when one is due, but no slice ends for it
  void update_soft_mask() {
    constexpr u32 timers = (1u << static_cast<u32>(EventId::Timer0)) | (1u << static_cast<u32>(EventId::Timer1)) | (1u << static_cast<u32>(EventId::Timer2)) | (1u << static_cast<u32>(EventId::Timer3)) |
                           (1u << static_cast<u32>(EventId::Timer7_0)) | (1u << static_cast<u32>(EventId::Timer7_1)) | (1u << static_cast<u32>(EventId::Timer7_2)) | (1u << static_cast<u32>(EventId::Timer7_3));
    soft_mask_ = (shift9_ && quantum_ <= LOCKSTEP_QUANTUM) ? timers : 0;
    rescan();
  }
  u64  slice_end_ = 0;      // the running slice's end (now_ + slice)
  s32  budget9_ = 0;        // the ARM9's budget for the running slice, as cut
  void cut_arm9_at(u64 at);
  void fire_due();
  void count_slice(bool skipped, s64 slice) const;
  // Idle-loop skip: an awake CPU sitting in a proven side-effect-free poll
  // loop is treated as halted for the slice. Sets which CPUs to skip and
  // returns whether the whole machine is idle. See cpu/idle_loop.h.
  bool machine_idle(bool& skip9, bool& skip7) const;
  // ARM7-only sleep: the ARM7 sitting in a proven poll loop on SPICNT while
  // a transfer is in flight (touch/mic sampling: Spirit Tracks does ~9 k
  // such polls a frame, each a slow-path I/O read) is treated as halted for
  // the slice while the ARM9 runs on; `wake` receives the transfer's ready
  // time so the slice can end there.
  bool arm7_spi_poll(u64& wake) const;
  mutable u32 spi_pc_ring_[8] = {};
  mutable u32 spi_pc_pos_ = 0;
  // Rings of each CPU's recent slice-start PCs. A slice ends at an arbitrary
  // point inside a loop, so "still in the loop" is membership in the last
  // few, not equality with the last one. The first ring is the idle-skip
  // pre-filter; the second is the DS_PROFILE spin proxy (a CPU re-entering
  // the same few addresses while awake is polling -- what both_idle() cannot see).
  mutable u32 idle_pc_ring_[2][8] = {};
  mutable u32 idle_pc_pos_[2] = {};
  mutable u32 spin_ring_[2][8] = {};
  mutable u32 spin_pos_[2] = {};
  mutable bool spin_now_[2] = {};
  static inline std::map<u32, u32>* spin_opcodes_ = nullptr;
  static inline std::map<u32, const char*>* spin_reject_ = nullptr;   // this slice's classification, for host-time attribution
  // Both CPUs halted with nothing pending that could wake them before the
  // next event: the slice can run to the deadline instead of the quantum.
  bool both_idle() const;
  void run_cpu(CpuContext& cpu, RunFn run);
};

} // namespace ds
