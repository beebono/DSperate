// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A sampling profiler for devices with no perf (the A30's 3.4 kernel has no
// perf events at all). ITIMER_PROF delivers SIGPROF on the process's CPU time
// -- at the kernel's tick, 100 Hz there -- to whichever thread is running, and
// the handler records the interrupted PC and that thread's id. Every thread is
// sampled, so the profile is the whole process: the emulation thread, the
// geometry worker and the band workers, told apart by the thread column.
//
// The output file, for tools/pc_profile.py:
//   base  <load address of the executable>
//   pid   <process id>                      (DS_PERF_MAP=1 names JIT blocks in /tmp/perf-<pid>.map)
//   obj   <start> <end> <path>              (each executable segment of each loaded object)
//   window <seconds> <clock ticks per second>   (wall time since the first activation)
//   thread <tid> <name> <cpu ticks>         (utime+stime over that window; threads alive at write time)
//   <pc> <tid>                              (one line per sample)
// PCs in no object are JIT code (or other anonymous executable memory).
#pragma once
#include <cstdint>

namespace ds::pcsample {

// Installs the handler and starts the timer. Samples are only kept while
// active; start() leaves it inactive.
bool start();
// Whether a tick that lands now is recorded. Cheap: one atomic store.
void set_active(bool on);
// Stops the timer and writes the profile. False when the file cannot be
// written (or the platform has no sampler).
bool write(const char* path);

} // namespace ds::pcsample
