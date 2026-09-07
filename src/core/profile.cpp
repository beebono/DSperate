// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/profile.h"

#include <algorithm>
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
  "jit translate",
};

const char* const count_names[] = {"3d polygon lines", "3d span pixels", "3d resolved pixels",
  "3d texel gathers (cached/direct)", "3d texel gathers (compressed)", "3d texel gathers (via views)",
  "3d texcache validated", "3d texcache decoded", "3d texcache bytes compared", "3d frames kept (no new swap)",
  "slices", "slices arm9 halted", "slices arm7 halted", "slices both halted", "slices with dma", "slices run to the deadline (both halted)",
  "cycles total", "cycles both halted", "cycles arm9 halted only", "cycles arm7 halted only", "cycles neither halted",
  "cycles arm9 awake+spinning", "cycles arm7 awake+spinning", "cycles one spinning, other halted", "cycles all idle (halt or spin)",
  "host ns arm9 in spin slices", "host ns arm7 in spin slices", "host ns arm9 working", "host ns arm7 working", "cycles skipped by idle-loop detect", "idle veto: dma", "idle veto: gx busy", "idle veto: irq pending", "idle veto: pc filter", "idle veto: arm9 not a loop", "idle veto: arm7 not a loop", "idle skip allowed",
  "2d lines rendered", "2d text bg lines", "2d affine bg lines", "2d extended bg lines", "2d 3d-layer lines", "2d lines with sprites", "2d lines with windows", "2d lines with colour effect", "2d lines where an effect can apply", "2d flat lines (no effect possible)", "2d plane selects",
  "2d lines: 0 layers", "2d lines: 1 layer", "2d lines: 2 layers", "2d lines: 3 layers", "2d lines: 4+ layers", "2d lines: 1 layer, fully opaque", "2d lines with obj pixels", "2d lines with 3d pixels", "2d lines with a window", "2d bg lines 16-colour text", "2d bg lines 256-colour text", "2d bg lines direct colour", "2d bg lines empty (transparent row)", "2d 3d-layer lines with nothing visible", "2d fast lines: backdrop only", "2d fast lines: one opaque layer", "2d full lines: effect mode live", "2d full lines: translucent 3d", "2d full lines: semi/bitmap sprites", "2d full lines: second target needed", "2d full lines: fade only", "3d spans: constant colour", "3d spans: interpolated colour", "3d band 0 ns", "3d band 1 ns", "3d band 2 ns", "3d band 3 ns", "3d band phase ns (slowest band)", "3d band ns summed (all bands)", "async probe: frames measured", "async probe: texture vram changed by next line 0", "async probe: texture vram changed by next swap", "async probe: vramcnt rewritten by next line 0", "async probe: vramcnt rewritten by next swap", "3d raster force-joined (vram touched)", "3d bins drawn by the thread that would have waited", "3d frames at 1 worker", "3d frames at 2 workers", "3d frames at 3 workers", "3d frames at 4 workers", "3d batches flushed", "3d spans batched", "3d pixels batched",
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
  "2d lazy frames", "2d lazy frames skipped (futile)", "2d vram trap hits", "2d lag frames", "2d lag: trapped stores", "2d lag: stores that joined a line", "2d lag: frames that hit the trap limit", "2d lag: lines left in flight", "2d engine A batch: stores that joined it", "2d engine A batch: capture-bank reads that joined it",
  "3d resolve kernel calls", "3d resolve parts entered",
  "3d polygon-chunk entries",
  "3d spans empty (no pixels)", "3d spans fully occluded by depth", "3d spans that draw",
  "gx reg reads", "gx reads of GXSTAT", "gx GXSTAT reads while busy (bit27)", "gx GXSTAT reads with pipe non-empty", "gx GXSTAT reads with fifo non-empty", "gx run_to_slow calls", "gx run_to_slow calls that executed",
  "3d drawn spans: plain", "3d drawn pixels: plain", "3d drawn spans: toon/highlight", "3d drawn pixels: toon/highlight", "3d drawn spans: shadow (scalar)", "3d drawn pixels: shadow (scalar)", "3d drawn spans: wireframe (scalar)", "3d drawn pixels: wireframe (scalar)",
  "2d compares: bg palette (512B)", "2d compares: bg ext palette (512B)", "2d compares: obj palette (512B)", "2d compares: obj ext palette (512B)", "2d compares: oam (1024B)",
  "2d compares that differed: bg palette", "2d compares that differed: bg ext palette", "2d compares that differed: obj palette", "2d compares that differed: obj ext palette", "2d compares that differed: oam",
  "dma transfers started", "dma dispatch loop entries (a run or one unit)",
  "dma gxfifo words (run)", "dma gxfifo words (per word)", "dma gxfifo bulk runs",
  "dma page-to-page runs", "dma units in word runs", "dma units in halfword runs", "dma words through the bus", "dma halfwords through the bus",
  "dma vram traps taken for a run",
  "dma units -> main ram", "dma units -> wram", "dma units -> palette", "dma units -> oam", "dma units -> i/o", "dma units -> elsewhere",
  "dma units -> vram engine A bg", "dma units -> vram engine B bg", "dma units -> vram engine A obj", "dma units -> vram engine B obj", "dma units -> vram lcdc",
  "dma vram traps: engine A bg", "dma vram traps: engine B bg", "dma vram traps: engine A obj", "dma vram traps: engine B obj", "dma vram traps: lcdc",
  "2d render ranges: engine A", "2d render ranges: engine B",
  "dma starts: immediate", "dma starts: vblank", "dma starts: hblank", "dma starts: display start", "dma starts: display fifo", "dma starts: cart", "dma starts: gba", "dma starts: gxfifo", "dma starts: arm7",
  "3d resolve groups (8px)", "3d resolve groups: nothing drawn", "3d resolve groups: one kind", "3d resolve groups: mixed kinds", "3d resolve groups: opaque only", "3d resolve groups: translucent only", "3d resolve groups: touching the under layer",
  "3d resolve batches", "3d resolve batches: nothing drawn", "3d resolve batches: one kind", "3d resolve batches: mixed kinds", "3d resolve batches: opaque only",
  "3d resolve: drawing groups in batches", "3d resolve: drawing groups in one-kind batches", "3d resolve: drawing groups in mixed batches",
  "3d resolve empty groups: under-layer candidates only", "3d resolve empty groups: a top lane passed depth",
  "3d resolve groups: every lane opaque and drawing", "3d resolve pixels in every-lane-opaque groups",
  "jit retime invalidations", "jit blocks killed by retimes",
  "slices arm9 gx-stalled",
  "bus vram remaps", "bus tcm updates", "bus gba slot retimes", "timing cpu9 range rebuilds"};

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

// Per-frame stage series. Only the emulation thread's stages are kept per
// frame: it waits for the slowest band (R3D_WAIT), so its stages sum to the
// frame's critical path and can be compared against the frontend's wall-clock
// frame_ms. The band workers' time is folded into one "band workers" column
// for context -- it overlaps the wait, it does not add to the wall time.
// Reading the workers' counters here is a census, not a synchronisation:
// a torn read costs one misattributed nanosecond slice, not a wrong answer.
namespace {
struct FrameNs { u64 ns[COUNT]; u64 workers; };
std::vector<FrameNs> frame_series;
u64 frame_last_ns[COUNT];
u64 frame_last_workers;
} // namespace

void frame_mark() {
  if (!enabled) return;
  u64 now[COUNT] = {}; u64 workers = 0;
  {
    std::lock_guard<std::mutex> lk(detail::accs_mutex());
    for (const Accum* a : detail::accs()) {
      if (a->tid == 0) for (u32 i = 0; i < COUNT; ++i) now[i] = a->ns[i];
      else             for (u32 i = 0; i < COUNT; ++i) workers += a->ns[i];
    }
  }
  FrameNs d{};
  for (u32 i = 0; i < COUNT; ++i) { d.ns[i] = now[i] - frame_last_ns[i]; frame_last_ns[i] = now[i]; }
  d.workers = workers - frame_last_workers;
  frame_last_workers = workers;
  frame_series.push_back(d);
}

void frame_breakdown(const std::vector<double>& frame_ms) {
  if (!enabled || frame_series.empty() || frame_series.size() != frame_ms.size()) return;
  const size_t n = frame_ms.size();
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return frame_ms[a] < frame_ms[b]; });
  // p99 group: the slowest 1% (at least one frame). typical group: the middle
  // fifth, so both boot transients and the spikes themselves stay out of the
  // baseline.
  const size_t n99 = std::max<size_t>(1, n / 100);
  const std::vector<size_t> tail(order.end() - static_cast<long>(n99), order.end());
  const std::vector<size_t> mid(order.begin() + static_cast<long>(n * 2 / 5),
                                order.begin() + static_cast<long>(n * 3 / 5));
  struct Group { double ms = 0, stage[COUNT] = {}, workers = 0, untimed = 0; };
  auto mean = [&](const std::vector<size_t>& idx) {
    Group g;
    for (size_t i : idx) {
      double timed = 0;
      for (u32 s = 0; s < COUNT; ++s) { const double v = frame_series[i].ns[s] / 1e6; g.stage[s] += v; timed += v; }
      g.workers += frame_series[i].workers / 1e6;
      g.ms += frame_ms[i];
      g.untimed += frame_ms[i] - timed;
    }
    const double k = 1.0 / static_cast<double>(idx.size());
    g.ms *= k; g.workers *= k; g.untimed *= k;
    for (u32 s = 0; s < COUNT; ++s) g.stage[s] *= k;
    return g;
  };
  const Group m = mean(mid), t = mean(tail);
  std::fprintf(stderr, "[frames] stage breakdown, mean of typical (middle 20%%, %zu frames) vs p99 tail (%zu frames), sorted by what the tail adds:\n",
               mid.size(), tail.size());
  std::fprintf(stderr, "[frames] %-16s %9s %9s %9s\n", "stage", "typ ms", "p99 ms", "delta");
  std::vector<u32> rows(COUNT);
  for (u32 s = 0; s < COUNT; ++s) rows[s] = s;
  std::sort(rows.begin(), rows.end(), [&](u32 a, u32 b) { return t.stage[a] - m.stage[a] > t.stage[b] - m.stage[b]; });
  for (u32 s : rows)
    if (m.stage[s] >= 0.0005 || t.stage[s] >= 0.0005)
      std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %+9.3f\n", names[s], m.stage[s], t.stage[s], t.stage[s] - m.stage[s]);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %+9.3f   (frame_ms minus timed stages: JIT translate, event/bus work between scopes, sched)\n",
               "untimed", m.untimed, t.untimed, t.untimed - m.untimed);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %+9.3f   (overlaps the band wait; not part of the wall time)\n",
               "band workers", m.workers, t.workers, t.workers - m.workers);
  std::fprintf(stderr, "[frames] %-16s %9.3f %9.3f %+9.3f\n", "frame total", m.ms, t.ms, t.ms - m.ms);
  // The worst individual frames, each with its heaviest stages: clusters with
  // one cause look alike here, mixed causes do not.
  const size_t worst_n = std::min<size_t>(6, n);
  std::fprintf(stderr, "[frames] worst %zu frames:\n", worst_n);
  for (size_t k = 0; k < worst_n; ++k) {
    const size_t i = order[n - 1 - k];
    double timed = 0;
    std::vector<u32> top(COUNT);
    for (u32 s = 0; s < COUNT; ++s) { top[s] = s; timed += frame_series[i].ns[s] / 1e6; }
    std::sort(top.begin(), top.end(), [&](u32 a, u32 b) { return frame_series[i].ns[a] > frame_series[i].ns[b]; });
    std::fprintf(stderr, "[frames]   #%-5zu %7.3f ms:", i, frame_ms[i]);
    for (size_t s = 0; s < 3 && frame_series[i].ns[top[s]]; ++s)
      std::fprintf(stderr, " %s %.3f", names[top[s]], frame_series[i].ns[top[s]] / 1e6);
    std::fprintf(stderr, " untimed %.3f\n", frame_ms[i] - timed);
  }
}

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
