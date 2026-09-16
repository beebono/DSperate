// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once

#include <cstdio>
#include <string>

namespace ds::sdl {

// What decides the host's CPU frequency, and whether it decides it by
// polling how busy the last few milliseconds looked.
//
// The distinction matters because an emulator's load is duty-cycled and a
// poller cannot see that. A frame of work followed by a sleep to the next
// deadline reads as ~60 % busy at full clock -- so `ondemand`, whose whole
// model is "raise the clock when a sample window was nearly all busy",
// never raises it, and its sample windows (8 ms against a 16.7 ms frame on
// ROCKNIX) sometimes land entirely inside the sleep and read as idle, which
// steps the clock *down*. The next frame then starts at 600-1100 MHz and
// takes two or three times its budget, which is heard as a gap in the sound
// rather than seen as a slow frame.
//
// Measured on the RG DS Plus (RK3566, ROCKNIX's ondemand: up_threshold 95,
// sampling_rate 8 ms), SM64DS replay, 1200 frames, both orders:
//
//                    median      p99      over budget   dry audio frames
//   performance    9.7-10.4   19.2-19.6      26-35            3-7
//   schedutil     10.0-10.4   19.1-19.2      29-31            6-7
//   ondemand      11.1-12.1   24.7-35.6      77-162         101-140
//
// The mean cost is ~1.5 ms, but the tail is 4-6x the missed frames and ~20x
// the audio gaps, and it is not thermal (54-57 C throughout) nor the average
// clock (36 % of the run *is* at 1992 MHz). It is the dives. Raising
// up_threshold's reach does not help -- forcing 89 % residency at maximum
// with up_threshold=40 left the tail worse, because the dives remain and
// each costs a 171 us transition plus up to a sample window at the low
// clock.
//
// `performance` and `schedutil` both measure free: schedutil is driven by
// the scheduler's own utilisation signal, which decays across a sleep
// instead of resetting, so a duty-cycled load holds its estimate.
inline std::string read_policy_attr(unsigned policy, const char* attr) {
  char path[128];
  std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%u/%s", policy, attr);
  std::FILE* f = std::fopen(path, "r");
  if (!f) return {};
  char v[64] = {};
  const bool ok = std::fgets(v, sizeof v, f) != nullptr;
  std::fclose(f);
  if (!ok) return {};
  std::string s(v);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

// The governor of one cpufreq policy, or "" when there is none to read
// (no cpufreq at all, or a policy number that does not exist).
inline std::string cpu_governor(unsigned policy = 0) { return read_policy_attr(policy, "scaling_governor"); }

// True for the governors that decide the clock by sampling how busy the last
// window looked.
//
// `powersave` is in the list because it pins the slowest OPP, which leaves a
// player in the same position -- but only under a driver that really means
// that by it. Under `intel_pstate` and `amd-pstate` the name is something
// else entirely: the hardware picks the P-state from its own view of
// utilisation (an energy/performance *preference*, not a fixed floor), it is
// the default on most desktops, and it does not have this problem. Spinning
// a core there would be a plain waste, so the driver has to agree.
inline bool governor_is_polling(const std::string& g, const std::string& driver = {}) {
  if (g == "ondemand" || g == "conservative" || g == "interactive") return true;
  return g == "powersave" && driver.find("pstate") == std::string::npos;
}

// Whether *any* cpufreq policy on this machine is run by a poller -- the
// emulator's threads are spread over every core, so one polled policy is
// enough to hold a frame up. Names the first one found.
inline bool host_governor_polls(std::string* which = nullptr) {
  for (unsigned p = 0; p < 16; ++p) {
    const std::string g = cpu_governor(p);
    if (g.empty()) continue;         // a gap in the numbering is not the end
    if (governor_is_polling(g, read_policy_attr(p, "scaling_driver"))) { if (which) *which = g; return true; }
  }
  return false;
}

} // namespace ds::sdl
