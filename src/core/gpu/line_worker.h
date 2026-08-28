// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One worker thread that runs a fixed job on request, for engine B's share of
// a display line while the calling thread renders engine A's.
//
// The handoff has to be cheap: it happens once per display line, 192 times a
// frame, against a job of tens of microseconds. A condition variable costs
// more to wake than the job saves at that granularity -- DraStic sidesteps
// this by deferring both engines and handing off once a frame instead
// (video_render_scanlines takes the threaded path only when the whole frame
// is still pending), which is not open to us: the per-line 2D pass is what
// lets the 3D band raster stay pipelined against the display, so the lines
// cannot be batched to the end of the frame.
//
// So the wait spins first and only falls back to sleeping once the spin
// budget is gone. A run of display lines keeps the worker hot and every
// handoff stays in the spin path; when the emulator stops asking (VBlank, or
// a paused frontend) it parks on the condition variable within a few
// microseconds rather than burning a core. This matters on the 4-core
// handhelds, where the 3D band workers already want three of them.
#pragma once

#include "core/types.h"

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ds::gpu {

class LineWorker {
public:
  LineWorker() = default;
  ~LineWorker() { stop(); }
  LineWorker(const LineWorker&) = delete;
  LineWorker& operator=(const LineWorker&) = delete;

  // `fn`/`arg` is the job; it runs on the worker for every dispatch().
  void start(void (*fn)(void*), void* arg) {
    if (thread_.joinable()) return;
    fn_ = fn; arg_ = arg;
    quit_.store(false, std::memory_order_relaxed);
    req_.store(0, std::memory_order_relaxed);
    ack_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    if (!thread_.joinable()) return;
    quit_.store(true, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(m_); }
    cv_.notify_all();
    thread_.join();
  }

  bool running() const { return thread_.joinable(); }
  // Hand-off state, for a stalled-frame watchdog (DS_WATCHDOG).
  void debug_dump(FILE* f) const {
    std::fprintf(f, "  line worker: running %d req %u ack %u parked %d\n", running() ? 1 : 0,
                 req_.load(std::memory_order_relaxed), ack_.load(std::memory_order_relaxed), parked_.load(std::memory_order_relaxed) ? 1 : 0);
  }

  // Hand the job over. Must be paired with wait() before the job's output is
  // read, and before the next dispatch().
  void dispatch() {
    const u32 n = req_.load(std::memory_order_relaxed) + 1;
    // Sequentially consistent, and it has to be. This and the worker's park
    // are a Dekker pair -- one stores req_ then reads parked_, the other
    // stores parked_ then reads req_ -- and release/acquire on *different*
    // objects does not order a store followed by a load. With the weaker
    // ordering both sides could read stale: the dispatch sees parked_ false
    // and skips the notify while the worker sees the old req_ and sleeps,
    // and wait() then spins on ack_ for ever. That is not theoretical; it
    // hung a 1800-frame run with the emulation thread at 99.9 % CPU and every
    // worker blocked on a futex.
    req_.store(n, std::memory_order_seq_cst);
    // The fence is for qemu-user. On AArch64 the compiler emits stlr/ldar for
    // seq_cst and leans on the architecture's rule that an ldar cannot pass an
    // earlier stlr; qemu's TCG models the pair as plain release/acquire, so on
    // an x86 host the store and the load below reorder and the Dekker check
    // fails -- caught by DS_WATCHDOG: req one ahead of ack, worker parked, in
    // 1 of ~6 headless dbori runs. Real hardware never needs it; one dmb per
    // display line is nothing.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // Only take the lock when the worker may already have parked. Spinning
    // workers see the store without it, so the common handoff is lock-free.
    if (parked_.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lk(m_);
      cv_.notify_one();
    }
  }

  // Spin briefly, then yield. Spinning alone is right only when a core is
  // free; when the 3D band workers are saturating the machine (heavy overdraw
  // scenes) the caller holding a core to spin on is taking it from the very
  // threads it is waiting behind. The budget covers a job that is already
  // nearly done, which is the case whenever engine A and engine B are close
  // in cost.
  void wait() {
    const u32 n = req_.load(std::memory_order_relaxed);
    int spins = 4000;
    while (ack_.load(std::memory_order_acquire) != n) {
      if (--spins > 0) cpu_relax();
      else std::this_thread::yield();
    }
  }

private:
  static void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  void loop() {
    u32 last = 0;
    for (;;) {
      // Spin for roughly the gap between two display lines before parking, so
      // a frame's worth of dispatches never pays a wakeup.
      constexpr int kSpin = 20000;
      int spins = kSpin;
      u32 r = req_.load(std::memory_order_acquire);
      while (r == last) {
        if (quit_.load(std::memory_order_relaxed)) return;
        if (--spins > 0) { cpu_relax(); r = req_.load(std::memory_order_acquire); continue; }
        std::unique_lock<std::mutex> lk(m_);
        parked_.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);   // see dispatch()
        // Re-check after publishing parked_, with the same total order the
        // dispatch side uses: a dispatch that raced the park would have seen
        // parked_ false and skipped the notify, so it must be caught here.
        r = req_.load(std::memory_order_seq_cst);
        if (r == last && !quit_.load(std::memory_order_relaxed))
          cv_.wait(lk, [&] {
            r = req_.load(std::memory_order_acquire);
            return r != last || quit_.load(std::memory_order_relaxed);
          });
        parked_.store(false, std::memory_order_seq_cst);
        if (quit_.load(std::memory_order_relaxed)) return;
        spins = kSpin;
      }
      last = r;
      fn_(arg_);
      ack_.store(r, std::memory_order_release);
    }
  }

  std::thread thread_;
  void (*fn_)(void*) = nullptr;
  void* arg_ = nullptr;
  std::atomic<u32> req_{0}, ack_{0};
  std::atomic<bool> quit_{false}, parked_{false};
  std::mutex m_;
  std::condition_variable cv_;
};

} // namespace ds::gpu
