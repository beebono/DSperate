// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/host_cores.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace ds {

namespace {

constexpr u32 MAX_CPUS = 64;

int forced_cores() {
  static const int n = [] { const char* e = std::getenv("DS_HOST_CORES"); return e ? std::atoi(e) : 0; }();
  return n;
}

#if defined(__linux__)
// "0,3" or "0-3,6": the kernel's online list. 0 when unreadable.
u64 read_online_mask() {
  FILE* f = std::fopen("/sys/devices/system/cpu/online", "r");
  if (!f) return 0;
  char buf[256];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  u64 mask = 0;
  for (char* p = buf; *p;) {
    char* end;
    const unsigned long lo = std::strtoul(p, &end, 10);
    if (end == p) break;
    unsigned long hi = lo;
    if (*end == '-') hi = std::strtoul(end + 1, &end, 10);
    for (unsigned long c = lo; c <= hi && c < MAX_CPUS; ++c) mask |= u64{1} << c;
    p = *end == ',' ? end + 1 : end;
    if (*p == '\n') break;
  }
  return mask;
}

// The process's affinity as first read -- always before a DS_PIN_THREADS pin
// narrows the calling thread's own, since pinning reads the usable set first.
// A taskset or cpuset is a standing restriction.
u64 startup_affinity() {
  static const u64 mask = [] {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) return ~u64{0};
    u64 m = 0;
    for (u32 c = 0; c < MAX_CPUS; ++c) if (CPU_ISSET(c, &set)) m |= u64{1} << c;
    return m ? m : ~u64{0};
  }();
  return mask;
}

// The usable set, re-read at most once a second.
u64 usable_mask() {
  static std::atomic<u64> cached{0};
  static std::atomic<s64> read_at{0};
  const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  u64 m = cached.load(std::memory_order_relaxed);
  if (!m || now - read_at.load(std::memory_order_relaxed) >= 1000) {
    const u64 aff = startup_affinity();
    const u64 online = read_online_mask();
    m = online ? (online & aff) : aff;
    if (!m) m = online ? online : 1;
    cached.store(m, std::memory_order_relaxed);
    read_at.store(now, std::memory_order_relaxed);
  }
  return m;
}
#endif

} // namespace

u32 host_cores() {
  if (forced_cores() > 0) return static_cast<u32>(forced_cores());
#if defined(__linux__)
  const u64 m = usable_mask();
  if (m != ~u64{0}) return static_cast<u32>(__builtin_popcountll(m));
#endif
  const u32 hc = std::thread::hardware_concurrency();
  return hc ? hc : 4u;
}

bool pin_threads() {
  static const bool on = [] { const char* e = std::getenv("DS_PIN_THREADS"); return e && std::atoi(e) != 0; }();
  return on;
}

void pin_current_thread(u32 k) {
#if defined(__linux__)
  // The k-th usable CPU, wrapping: the online set need not be contiguous
  // (spruce's powersave on the A30 leaves cpus 0 and 3).
  const u64 m = usable_mask();
  const u32 count = m == ~u64{0} ? 0 : static_cast<u32>(__builtin_popcountll(m));
  if (!count) return;
  u32 want = k % count, cpu = 0;
  for (; cpu < MAX_CPUS; ++cpu) if ((m >> cpu) & 1) { if (want == 0) break; --want; }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpu), &set);
  sched_setaffinity(0, sizeof set, &set);   // a hint: failure leaves the thread where it was
#else
  (void)k;
#endif
}

} // namespace ds
