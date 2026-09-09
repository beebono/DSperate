// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What the achievement runtime costs per frame, on the device that has to pay
// it (docs/retroachievements-scoping.md, phase 2).
//
// This is measured before any networking exists, and deliberately so: if
// evaluating a full achievement set is expensive on an A55 then the whole
// feature needs a different shape, and that is much cheaper to learn now than
// after the session code is written.
//
// What is measured is rc_runtime_do_frame over our memory window -- the
// condition evaluation itself. rc_client_do_frame (phase 3) wraps exactly this
// and adds only bookkeeping, so this is the part that scales with the set and
// the part worth knowing.
//
// The set is synthetic because we cannot download a real one yet. It is sized
// to be pessimistic rather than typical: see below.
//
//   tools/cheevos_bench [achievements] [conditions each] [frames]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cheevos/cheevos_memory.h"
#include "core/mem/bus.h"
#include "core/nds.h"

extern "C" {
#include "rc_runtime.h"
}

using namespace ds;

namespace {

u64 now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

int g_triggered = 0;
void on_event(const rc_runtime_event_t* e) {
  if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) ++g_triggered;
}

// One synthetic achievement: `conds` conditions over addresses spread across
// main RAM, mixing plain reads with delta reads (which cost an extra stored
// value and a comparison) and sizes, because a real set does. The conditions
// are built so the achievement does not actually trigger -- a triggered
// achievement is removed from the active list and would quietly make the
// benchmark faster as it ran.
std::string make_achievement(u32 index, u32 conds) {
  std::string s;
  for (u32 c = 0; c < conds; ++c) {
    if (c) s += '_';
    // Addresses are spread over the first 1 MB, which is where DS game state
    // actually lives, and deliberately overlap between achievements so that
    // rcheevos' memref dedup gets exercised the way it would in a real set.
    const u32 addr = ((index * 37 + c * 4099) % 0x100000) & ~3u;
    char buf[64];
    switch (c % 4) {
      case 0: std::snprintf(buf, sizeof buf, "0xH%06X=255", addr); break;          // 8-bit
      case 1: std::snprintf(buf, sizeof buf, "0x %06X=65535", addr); break;        // 16-bit
      case 2: std::snprintf(buf, sizeof buf, "0xX%06X=4294967295", addr); break;   // 32-bit
      default: std::snprintf(buf, sizeof buf, "d0xH%06X=254", addr); break;        // delta
    }
    s += buf;
  }
  return s;
}

} // namespace

int main(int argc, char** argv) {
  const u32 count  = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 120;
  const u32 conds  = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 6;
  const u32 frames = argc > 3 ? static_cast<u32>(std::atoi(argv[3])) : 3600;

  NDS nds;
  cheevos::Memory mem;
  std::string err;
  if (!mem.attach(nds, err)) {
    std::fprintf(stderr, "cheevos_bench: %s\n", err.c_str());
    return 1;
  }

  rc_runtime_t rt;
  rc_runtime_init(&rt);
  u32 activated = 0;
  for (u32 i = 0; i < count; ++i) {
    const std::string expr = make_achievement(i, conds);
    if (rc_runtime_activate_achievement(&rt, i + 1, expr.c_str(), nullptr, 0) == RC_OK) ++activated;
    else std::fprintf(stderr, "cheevos_bench: rejected %s\n", expr.c_str());
  }

  // Churn memory so the runtime is not reading a static image: a real game
  // rewrites its state every frame, and the delta conditions depend on it.
  u8* ram = nds.bus.main_ram.get();
  u32 seed = 12345;

  std::vector<u64> ns(frames);
  for (u32 f = 0; f < frames; ++f) {
    for (u32 i = 0; i < 4096; ++i) {            // ~4 KB of change per frame
      seed = seed * 1664525u + 1013904223u;
      ram[(seed >> 8) & 0xFFFFF] = static_cast<u8>(seed >> 24);
    }
    const u64 t0 = now_ns();
    rc_runtime_do_frame(&rt, on_event, cheevos::Memory::peek, &mem, nullptr);
    ns[f] = now_ns() - t0;
  }

  std::sort(ns.begin(), ns.end());
  const u64 total = [&] { u64 t = 0; for (u64 v : ns) t += v; return t; }();
  auto pct = [&](double p) { return ns[static_cast<size_t>(p * (frames - 1))]; };

  // A DS frame is 16.67 ms. The house rule is to quote p99 and the tail, not
  // just the mean (docs, check-p99-not-just-mean).
  std::printf("achievements %u (%u activated), %u conditions each, %u frames\n",
              count, activated, conds, frames);
  std::printf("do_frame  mean %.1f us  p50 %.1f us  p99 %.1f us  max %.1f us\n",
              total / 1000.0 / frames, pct(0.50) / 1000.0, pct(0.99) / 1000.0,
              ns.back() / 1000.0);
  std::printf("share of a 16.67 ms frame:  mean %.2f %%  p99 %.2f %%\n",
              total / static_cast<double>(frames) / 16666667.0 * 100.0,
              pct(0.99) / 16666667.0 * 100.0);
  std::printf("(triggered %d, which should be 0; a trigger would shrink the set)\n", g_triggered);
  rc_runtime_destroy(&rt);
  return 0;
}
