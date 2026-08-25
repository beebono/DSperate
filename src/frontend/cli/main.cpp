// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless CLI: boots the BIOS/firmware (and optionally a ROM), runs N frames,
// optionally writing per-CPU instruction traces in the shared trace format:
//   <pc> <instr> <cpsr> r0 .. r14     (hex, one line per instruction)
// and/or dumping raw framebuffers (--dump-frames) for tools/compare_frames.py
// and the SPU output (--dump-audio, raw s16 stereo at 32768 Hz).
#include "core/nds.h"
#include "core/cpu/interp/interp.h"
#include "core/input/input_log.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "core/profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>

namespace {
// Spin-loop collapse: a poll loop repeats its exact state every iteration until
// the polled value changes, so a line identical to one of the last 32 emitted
// lines is dropped. Both tracers apply the same rule, so traces stay comparable
// while shrinking by orders of magnitude.
struct TraceState {
  FILE* out[2] = {nullptr, nullptr};
  unsigned long long count[2] = {0, 0};
  unsigned long long executed[2] = {0, 0};
  unsigned long long max = 1'000'000;
  ds::u64 recent[2][32] = {};
  unsigned recent_pos[2] = {0, 0};
  bool pc_hist = false;                       // TRACE_PC_HIST=1: uncollapsed PC histogram on stderr at exit
  std::unordered_map<ds::u32, unsigned long long> hist[2];
};

void trace_cb(ds::CpuContext& cpu, ds::u32 instr, void* user) {
  auto* t = static_cast<TraceState*>(user);
  const int i = static_cast<int>(cpu.which);
  t->executed[i]++;
  if (t->pc_hist) t->hist[i][cpu.hot.regs[15] - (cpu.thumb() ? 4 : 8)]++;
  if (t->count[i] >= t->max) return;
  const bool thumb = cpu.thumb();
  const ds::u32 pc = cpu.hot.regs[15] - (thumb ? 4 : 8);
  ds::u64 h = 1469598103934665603ull;
  auto mix = [&](ds::u32 v) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; };
  mix(pc); mix(instr); mix(cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) mix(cpu.hot.regs[r]);
  for (int k = 0; k < 32; ++k) if (t->recent[i][k] == h) return;
  t->recent[i][t->recent_pos[i]++ & 31] = h;
  t->count[i]++;
  std::fprintf(t->out[i], "%08x %08x %08x", pc, instr, cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) std::fprintf(t->out[i], " %08x", cpu.hot.regs[r]);
  std::fputc('\n', t->out[i]);
}
} // namespace

int main(int argc, char** argv) {
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr, *trace = nullptr, *dump = nullptr, *dump_audio = nullptr, *replay = nullptr, *save = nullptr;
  // A whole 1800-frame dump is ~708 MB, so a window can be selected: the
  // frame-budget report below names the frames worth looking at.
  int dump_from = 0, dump_count = 0;
  int frames = 60; bool direct = false;
#if DSPERATE_JIT
  bool jit9 = true, jit7 = true;
  long quantum = ds::LOCKSTEP_QUANTUM;   // the harness compares against melonDS: lockstep unless asked otherwise
#else
  bool jit9 = false, jit7 = false;
  long quantum = ds::LOCKSTEP_QUANTUM;
#endif
  TraceState ts;
  bool frames_given = false;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--frames")) { frames = std::atoi(argv[++i]); frames_given = true; }
    else if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--trace")) trace = argv[++i];
    else if (arg("--max")) ts.max = std::strtoull(argv[++i], nullptr, 0);
    else if (arg("--dump-frames")) dump = argv[++i];
    else if (arg("--dump-from")) dump_from = std::atoi(argv[++i]);    // first frame to dump
    else if (arg("--dump-count")) dump_count = std::atoi(argv[++i]);  // how many (0 = to the end)
    else if (arg("--dump-audio")) dump_audio = argv[++i];   // raw s16 stereo, 32768 Hz
    else if (arg("--replay")) replay = argv[++i];           // inputs recorded by dsperate-sdl --record; sets --frames to its length unless given
    else if (arg("--save")) save = argv[++i];               // battery save to start from; loaded read-only, never written back
    else if (!std::strcmp(argv[i], "--direct")) direct = true;
    else if (!std::strcmp(argv[i], "--interp")) jit9 = jit7 = false;          // interpreter for both CPUs
    else if (arg("--quantum")) quantum = std::atol(argv[++i]);                // CPU interleave in ARM9 cycles; 0 = event-bound (the frontends' mode)
    else if (!std::strcmp(argv[i], "--jit9")) { jit9 = true; jit7 = false; }  // recompile the ARM9 only
    else if (!std::strcmp(argv[i], "--jit7")) { jit9 = false; jit7 = true; }
    else rom = argv[i];
  }
  std::fprintf(stderr, "DSperate 0.0.1 (%s%s)\n",
#if DSPERATE_JIT
              "jit",
#else
              "interp",
#endif
#if DSPERATE_NEON
              "+neon");
#else
              "");
#endif
  ds::NDS nds;
  if (bios9 && bios7 && fw) {
    if (!nds.load_bios(bios9, bios7, fw)) { std::fprintf(stderr, "could not load BIOS/firmware\n"); return 1; }
    nds.reset();
  } else {
    std::fprintf(stderr, "note: no --bios9/--bios7/--firmware given; running with empty BIOS\n");
  }
  if (rom && !nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  nds.sched.set_quantum(quantum);
  if (rom && direct) nds.setup_direct_boot();
  // A recording made with a save present only replays if the save is there:
  // the game otherwise stops to create one. Loaded in the same place the SDL
  // frontend loads it, and never written back -- this is a harness.
  if (save && nds.cart && !nds.cart->sram().empty()) {
    if (FILE* f = std::fopen(save, "rb")) {
      std::vector<ds::u8>& sram = nds.cart->sram();
      const size_t n = std::fread(sram.data(), 1, sram.size(), f);
      std::fclose(f);
      std::fprintf(stderr, "save: loaded %zu bytes from %s\n", n, save);
    } else {
      std::fprintf(stderr, "save: cannot read %s\n", save);
      return 1;
    }
  }
#if DSPERATE_JIT
  if ((jit9 || jit7) && !ds::jit::attach(nds, jit9, jit7)) return 1;
#else
  (void)jit9; (void)jit7;
#endif
  ds::prof::enabled = std::getenv("DS_PROFILE") != nullptr;
  if (const char* w = std::getenv("DS_WATCH")) nds.bus.enable_watch(static_cast<ds::u32>(std::strtoul(w, nullptr, 16)));
  if (trace) {
    ts.out[0] = std::fopen((std::string(trace) + ".arm9.trace").c_str(), "w");
    ts.out[1] = std::fopen((std::string(trace) + ".arm7.trace").c_str(), "w");
  }
  const char* per_frame = std::getenv("TRACE_PER_FRAME");
  unsigned long long last9 = 0, last7 = 0;
  FILE* dump_out = dump ? std::fopen(dump, "wb") : nullptr;
  if (dump && !dump_out) { std::fprintf(stderr, "could not open %s\n", dump); return 1; }
  ds::input::Log log;
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    if (!frames_given) frames = static_cast<int>(log.frames());
    std::fprintf(stderr, "replay: %u frames from %s\n", log.frames(), replay);
  }
  FILE* audio_out = dump_audio ? std::fopen(dump_audio, "wb") : nullptr;
  if (dump_audio && !audio_out) { std::fprintf(stderr, "could not open %s\n", dump_audio); return 1; }
  ts.pc_hist = std::getenv("TRACE_PC_HIST") != nullptr;
  const char* trace_start = std::getenv("TRACE_START_FRAME");   // suppress trace output before this frame
  const int trace_from = trace_start ? std::atoi(trace_start) : 0;
  // Per-frame host times. Whole-process wall clock on the device turned out to
  // spread 13 % run to run at a flat temperature, which buries any change
  // worth measuring; the median frame rejects the transient stalls that cause
  // it, and p90 still shows them if they matter.
  std::vector<double> frame_ms;
  frame_ms.reserve(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (trace && i == trace_from) {
      nds.trace = trace_cb; nds.trace_user = &ts;
#if DSPERATE_JIT
      ds::jit::set_trace(true);
#endif
    }
    if (log.reading()) { ds::input::Frame in; if (log.read(in)) ds::input::apply(nds, in); }
    nds.run_frame();
    if (dump_out && i >= dump_from && (dump_count <= 0 || i < dump_from + dump_count)) {
      // raw 0xAARRGGBB, top screen then bottom, 256x192 each, one record per frame
      std::fwrite(nds.gpu.framebuffer(0), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
      std::fwrite(nds.gpu.framebuffer(1), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
    }
    if (audio_out) {
      ds::s16 buf[2048 * 2]; size_t n;
      while ((n = nds.spu.take(buf, 2048)) != 0) std::fwrite(buf, 4, n, audio_out);
    } else nds.spu.drain();
    frame_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (per_frame && trace) { std::fprintf(stderr, "frame %d arm9 %llu arm7 %llu\n", i, ts.executed[0] - last9, ts.executed[1] - last7); last9 = ts.executed[0]; last7 = ts.executed[1]; }
  }
  if (dump_out) std::fclose(dump_out);
  if (audio_out) std::fclose(audio_out);
  if (ts.pc_hist) {
    for (int c = 0; c < 2; ++c) {
      std::vector<std::pair<ds::u32, unsigned long long>> v(ts.hist[c].begin(), ts.hist[c].end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::fprintf(stderr, "arm%d hottest pcs:", c ? 7 : 9);
      for (size_t k = 0; k < v.size() && k < 12; ++k) std::fprintf(stderr, " %08x:%llu", v[k].first, v[k].second);
      std::fputc('\n', stderr);
    }
  }
  if (trace) { std::fclose(ts.out[0]); std::fclose(ts.out[1]);
    std::fprintf(stderr, "arm9: %llu lines (%llu instrs), arm7: %llu lines (%llu instrs), cap %llu lines each\n",
                 ts.count[0], ts.executed[0], ts.count[1], ts.executed[1], ts.max); }
  ds::prof::report();
#if DSPERATE_JIT
  if (ds::prof::enabled && (jit9 || jit7)) ds::jit::report(stderr);
#endif
  ds::interp::census_report(nds.frame_count);
  if (!frame_ms.empty()) {
    std::vector<double> v = frame_ms;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double x : v) sum += x;
    const size_t n = v.size();
    auto pct = [&](double p) { return v[std::min(n - 1, static_cast<size_t>(p * n))]; };
    // A DS frame is 1/59.8261 s. A frame that takes longer than that is one the
    // emulator could not deliver in real time -- which is what a dropped frame
    // and an audio underrun actually are. The mean hides these completely: a
    // change can improve the mean while making the tail worse, and the tail is
    // what is felt. Report both.
    constexpr double BUDGET_MS = 1000.0 / 59.8261;
    size_t over = 0; for (double x : v) if (x > BUDGET_MS) ++over;
    std::fprintf(stderr, "frame ms: median %.3f mean %.3f p90 %.3f p99 %.3f max %.3f min %.3f total %.1f\n",
                 v[n / 2], sum / n, pct(0.90), pct(0.99), v.back(), v.front(), sum);
    std::fprintf(stderr, "frame budget: %zu of %zu frames over %.3f ms (%.2f%%)\n",
                 over, n, BUDGET_MS, 100.0 * static_cast<double>(over) / static_cast<double>(n));

    // Where the overruns are, not just how many. A count says the run stutters;
    // this says which frames to re-run with --dump-from/--dump-count and look
    // at. `frame_ms` is in run order, unlike the sorted copy above.
    if (over) {
      size_t worst_i = 0, bursts = 0, longest = 0, longest_at = 0, cur = 0, cur_at = 0;
      for (size_t i = 0; i < frame_ms.size(); ++i) {
        if (frame_ms[i] > frame_ms[worst_i]) worst_i = i;
        if (frame_ms[i] > BUDGET_MS) {
          if (cur == 0) { ++bursts; cur_at = i; }
          if (++cur > longest) { longest = cur; longest_at = cur_at; }
        } else cur = 0;
      }
      std::fprintf(stderr, "  worst frame #%zu at %.3f ms; %zu bursts, longest %zu frames from #%zu\n",
                   worst_i, frame_ms[worst_i], bursts, longest, longest_at);
      // Ten windows over the run: a flat row is steady load, a spike is a
      // section that chugs and is worth dumping.
      constexpr size_t W = 10;
      const size_t span = (frame_ms.size() + W - 1) / W;
      std::fprintf(stderr, "  over-budget per %zu-frame window:", span);
      for (size_t w = 0; w < W; ++w) {
        size_t c = 0;
        for (size_t i = w * span; i < std::min(frame_ms.size(), (w + 1) * span); ++i)
          if (frame_ms[i] > BUDGET_MS) ++c;
        std::fprintf(stderr, " %zu", c);
      }
      std::fputc('\n', stderr);
    }
  }
  std::fprintf(stderr, "ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
