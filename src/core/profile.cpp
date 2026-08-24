// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/profile.h"

#include <cstdio>
#include <mutex>
#include <vector>

#include "core/cpu/idle_loop.h"

namespace ds::prof {

bool enabled = false;
const char* const names[COUNT] = {
  "cpu arm9", "cpu arm7", "dma", "gx geometry",
  "2d bg draw", "2d obj draw", "2d window", "2d select", "2d effects", "output", "capture",
  "3d clear", "3d spans", "3d final pass", "spu",
};

const char* const count_names[C_COUNT] = {"3d polygon lines", "3d span pixels", "3d resolved pixels",
  "3d texel gathers (cached/direct)", "3d texel gathers (compressed)", "3d texel gathers (via views)",
  "3d texcache validated", "3d texcache decoded", "3d texcache bytes compared", "3d frames kept (no new swap)",
  "slices", "slices arm9 halted", "slices arm7 halted", "slices both halted", "slices with dma", "slices run to the deadline (both halted)",
  "cycles total", "cycles both halted", "cycles arm9 halted only", "cycles arm7 halted only", "cycles neither halted",
  "cycles arm9 awake+spinning", "cycles arm7 awake+spinning", "cycles one spinning, other halted", "cycles all idle (halt or spin)",
  "host ns arm9 in spin slices", "host ns arm7 in spin slices", "host ns arm9 working", "host ns arm7 working", "cycles skipped by idle-loop detect", "idle veto: dma", "idle veto: gx busy", "idle veto: irq pending", "idle veto: pc filter", "idle veto: arm9 not a loop", "idle veto: arm7 not a loop", "idle skip allowed",
  "2d lines rendered", "2d text bg lines", "2d affine bg lines", "2d extended bg lines", "2d 3d-layer lines", "2d lines with sprites", "2d lines with windows", "2d lines with colour effect", "2d lines where an effect can apply", "2d flat lines (no effect possible)", "2d plane selects",
  "2d lines: 0 layers", "2d lines: 1 layer", "2d lines: 2 layers", "2d lines: 3 layers", "2d lines: 4+ layers", "2d lines: 1 layer, fully opaque", "2d lines with obj pixels", "2d lines with 3d pixels", "2d lines with a window", "2d bg lines 16-colour text", "2d bg lines 256-colour text", "2d bg lines direct colour", "2d bg lines empty (transparent row)", "2d fast lines: backdrop only", "2d fast lines: one opaque layer", "2d full lines: effect mode live", "2d full lines: translucent 3d", "2d full lines: semi/bitmap sprites", "2d full lines: second target needed", "2d full lines: fade only"};

namespace detail {
thread_local Accum* acc = nullptr;

std::mutex& accs_mutex() { static std::mutex m; return m; }
std::vector<Accum*>& accs() { static std::vector<Accum*> v; return v; }

// One accumulator per thread, owned by the registry and never freed: a band
// worker may outlive or predecease `report`, and the totals must survive it
// either way (there are only ever a handful of threads).
Accum* make_acc() {
  Accum* a = new Accum();
  { std::lock_guard<std::mutex> lk(accs_mutex()); accs().push_back(a); }
  acc = a;
  return a;
}
} // namespace detail

// Sum every thread's accumulator. Called from the emulation thread after the
// band workers are idle, so a plain lock is enough.
void report() {
  Accum t;
  {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      for (u32 i = 0; i < COUNT; ++i) t.ns[i] += a->ns[i];
      for (u32 i = 0; i < C_COUNT; ++i) t.count[i] += a->count[i];
    }
  }
  const u64* ns = t.ns;
  const u64* count = t.count;
  u64 total = 0;
  for (u32 i = 0; i < COUNT; ++i) total += ns[i];
  if (!total) return;
  for (u32 i = 0; i < C_COUNT; ++i)
    if (count[i]) std::fprintf(stderr, "[profile] %-20s %12llu\n", count_names[i], static_cast<unsigned long long>(count[i]));
  {
    const auto& il = ds::cpu::idle_loop_stats();
    std::fprintf(stderr, "[profile] idle-loop: queries %llu hits %llu analyses %llu\n",
                 (unsigned long long)il.queries, (unsigned long long)il.hits, (unsigned long long)il.analyses);
    for (u32 r = 1; r < 12; ++r)
      if (il.by_reason[r])
        std::fprintf(stderr, "[profile]   reject %-14s %10llu\n",
                     ds::cpu::idle_reject_name(static_cast<ds::cpu::IdleReject>(r)),
                     (unsigned long long)il.by_reason[r]);
  }
  std::fprintf(stderr, "[profile] %-14s %9s %6s\n", "stage", "ms", "%");
  for (u32 i = 0; i < COUNT; ++i)
    if (ns[i]) std::fprintf(stderr, "[profile] %-14s %9.1f %5.1f%%\n", names[i], ns[i] / 1e6, 100.0 * ns[i] / total);
  std::fprintf(stderr, "[profile] %-14s %9.1f\n", "sum", total / 1e6);
}

} // namespace ds::prof
