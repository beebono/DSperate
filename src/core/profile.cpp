// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/profile.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "core/cpu/idle_loop.h"

namespace ds::prof {

bool enabled = false;
bool async_window = false;
bool census_same_list = false;
const char* const names[COUNT] = {
  "cpu arm9", "cpu arm7", "dma", "gx geometry",
  "2d bg draw", "2d obj draw", "2d window", "2d select", "2d effects", "output", "capture",
  "3d clear", "3d spans", "3d final pass", "3d band wait", "spu",
};

const char* const count_names[] = {"3d polygon lines", "3d span pixels", "3d resolved pixels",
  "3d texel gathers (cached/direct)", "3d texel gathers (compressed)", "3d texel gathers (via views)",
  "3d texcache validated", "3d texcache decoded", "3d texcache bytes compared", "3d frames kept (no new swap)",
  "slices", "slices arm9 halted", "slices arm7 halted", "slices both halted", "slices with dma", "slices run to the deadline (both halted)",
  "cycles total", "cycles both halted", "cycles arm9 halted only", "cycles arm7 halted only", "cycles neither halted",
  "cycles arm9 awake+spinning", "cycles arm7 awake+spinning", "cycles one spinning, other halted", "cycles all idle (halt or spin)",
  "host ns arm9 in spin slices", "host ns arm7 in spin slices", "host ns arm9 working", "host ns arm7 working", "cycles skipped by idle-loop detect", "idle veto: dma", "idle veto: gx busy", "idle veto: irq pending", "idle veto: pc filter", "idle veto: arm9 not a loop", "idle veto: arm7 not a loop", "idle skip allowed",
  "2d lines rendered", "2d text bg lines", "2d affine bg lines", "2d extended bg lines", "2d 3d-layer lines", "2d lines with sprites", "2d lines with windows", "2d lines with colour effect", "2d lines where an effect can apply", "2d flat lines (no effect possible)", "2d plane selects",
  "2d lines: 0 layers", "2d lines: 1 layer", "2d lines: 2 layers", "2d lines: 3 layers", "2d lines: 4+ layers", "2d lines: 1 layer, fully opaque", "2d lines with obj pixels", "2d lines with 3d pixels", "2d lines with a window", "2d bg lines 16-colour text", "2d bg lines 256-colour text", "2d bg lines direct colour", "2d bg lines empty (transparent row)", "2d 3d-layer lines with nothing visible", "2d fast lines: backdrop only", "2d fast lines: one opaque layer", "2d full lines: effect mode live", "2d full lines: translucent 3d", "2d full lines: semi/bitmap sprites", "2d full lines: second target needed", "2d full lines: fade only", "3d spans: constant colour", "3d spans: interpolated colour", "3d band 0 ns", "3d band 1 ns", "3d band 2 ns", "3d band 3 ns", "3d band phase ns (slowest band)", "3d band ns summed (all bands)", "async probe: frames measured", "async probe: texture vram changed by next line 0", "async probe: texture vram changed by next swap", "async probe: vramcnt rewritten by next line 0", "async probe: vramcnt rewritten by next swap", "3d raster force-joined (vram touched)", "3d batches flushed", "3d spans batched", "3d pixels batched",
  "gx swap_buffers (new list)", "gx swaps whose list is unchanged", "gx vblanks with no swap", "gx no-swap frames rejected by the register compare",
  "gx reject cause: dispcnt/alpha_ref", "gx reject cause: clear attrs", "gx reject cause: fog", "gx reject cause: edge/toon",
  "gx polygons submitted (summed over swaps)", "gx vertices submitted (summed over swaps)", "gx polygons in the largest swap", "gx vertices in the largest swap",
  "gx compare bytes if scanned in full", "gx compare bytes until the first difference", "gx compares run (counts matched)",
  "gx compare bytes, identical lists", "gx compares, identical lists", "gx compare bytes in full, differing lists", "gx compare bytes to first difference, differing lists", "gx compares, differing lists",
  "gx polygons in identical-list swaps", "gx vertices in identical-list swaps",
  "3d polygon lines in identical-list frames", "3d span pixels in identical-list frames",
  "3d spans len 1-4", "3d spans len 5-8", "3d spans len 9-16", "3d spans len 17-32", "3d spans len 33-64", "3d spans len 65-128", "3d spans len 129-256",
  "3d span pixels in len 1-4", "3d span pixels in len 5-8", "3d span pixels in len 9-16", "3d span pixels in len 17-32", "3d span pixels in len 33-64", "3d span pixels in len 65-128", "3d span pixels in len 129-256",
  "stores into palette space", "stores into oam space",
  "3d resolve kernel calls", "3d resolve parts entered",
  "3d polygon-chunk entries",
  "2d compares: bg palette (512B)", "2d compares: bg ext palette (512B)", "2d compares: obj palette (512B)", "2d compares: obj ext palette (512B)", "2d compares: oam (1024B)",
  "2d compares that differed: bg palette", "2d compares that differed: bg ext palette", "2d compares that differed: obj palette", "2d compares that differed: obj ext palette", "2d compares that differed: oam"};

static_assert(sizeof(count_names) / sizeof(*count_names) == C_COUNT,
              "count_names must have exactly one entry per Counter enumerator");

namespace detail {
thread_local Accum* acc = nullptr;

std::mutex& accs_mutex() { static std::mutex m; return m; }
std::vector<Accum*>& accs() { static std::vector<Accum*> v; return v; }

// One accumulator per thread, owned by the registry and never freed: a band
// worker may outlive or predecease `report`, and the totals must survive it
// either way (there are only ever a handful of threads).
Accum* make_acc() {
  Accum* a = new Accum();
  { std::lock_guard<std::mutex> lk(accs_mutex()); a->tid = static_cast<u32>(accs().size()); accs().push_back(a); }
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
  // DS_PROFILE_THREADS=1: the same stages per thread. The emulation thread
  // waits for the slowest band, so its own column is the frame's critical
  // path and the workers' columns are only what they contribute to it.
  if (std::getenv("DS_PROFILE_THREADS")) {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      u64 t = 0;
      for (u32 i = 0; i < COUNT; ++i) t += a->ns[i];
      if (!t) continue;
      std::fprintf(stderr, "[profile] --- thread %u (%s), %.1f ms of stage time\n", a->tid,
                   a->tid == 0 ? "emulation" : "band worker", t / 1e6);
      for (u32 i = 0; i < COUNT; ++i)
        if (a->ns[i]) std::fprintf(stderr, "[profile]     %-14s %9.1f %5.1f%%\n", names[i], a->ns[i] / 1e6, 100.0 * a->ns[i] / t);
      for (u32 i = 0; i < C_COUNT; ++i)
        if (a->count[i] && (i == C_POLY_LINES || i == C_SPAN_PIXELS || i == C_RESOLVED_PIXELS || i == C_2D_LINES))
          std::fprintf(stderr, "[profile]     %-24s %12llu\n", count_names[i], (unsigned long long)a->count[i]);
    }
  }
  std::fprintf(stderr, "[profile] %-14s %9s %6s\n", "stage", "ms", "%");
  for (u32 i = 0; i < COUNT; ++i)
    if (ns[i]) std::fprintf(stderr, "[profile] %-14s %9.1f %5.1f%%\n", names[i], ns[i] / 1e6, 100.0 * ns[i] / total);
  std::fprintf(stderr, "[profile] %-14s %9.1f\n", "sum", total / 1e6);
}

} // namespace ds::prof
