// Per-thread scheduling helper for the presenter threads.
//
// main.cpp puts the whole process under SCHED_RR (emu.realtime) before any
// thread exists, so every thread inherits one priority. On a device with
// fewer free cores than busy threads (the A30 runs two cores at idle) that
// leaves the presenter -- a thread that wakes on the vsync, flips, and
// sleeps -- queued behind the emulation thread and a band worker at equal
// priority: a round-robin slice on a 3.4 kernel is 100 ms, so the panel
// holds a stale frame for that long while the emulator runs flat out (an
// overlay-load translate burst, or --cpu-oc with no idle to sleep in). The
// presenter is cheap and latency-bound; it belongs one step above.
#pragma once

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#endif

namespace ds::sdl {

// Raise the calling thread one real-time priority step above what it
// inherited, if it inherited a real-time policy at all. No-op otherwise.
inline void raise_presenter_priority(const char* who) {
#if defined(__linux__)
  int policy = 0;
  sched_param sp{};
  if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return;
  if (policy != SCHED_RR && policy != SCHED_FIFO) return;
  const int max = sched_get_priority_max(policy);
  if (sp.sched_priority >= max) return;
  sp.sched_priority += 1;
  if (pthread_setschedparam(pthread_self(), policy, &sp) != 0) std::perror(who);
#else
  (void)who;
#endif
}

} // namespace ds::sdl
