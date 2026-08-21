// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless CLI: boots the BIOS/firmware (and optionally a ROM), runs N frames,
// optionally writing per-CPU instruction traces in the shared trace format:
//   <pc> <instr> <cpsr> r0 .. r14     (hex, one line per instruction)
// and/or dumping raw framebuffers (--dump-frames) for tools/compare_frames.py.
#include "core/nds.h"
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
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr, *trace = nullptr, *dump = nullptr;
  int frames = 60; bool direct = false;
#if DSPERATE_JIT
  bool jit9 = true, jit7 = true;
#else
  bool jit9 = false, jit7 = false;
#endif
  TraceState ts;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--frames")) frames = std::atoi(argv[++i]);
    else if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--trace")) trace = argv[++i];
    else if (arg("--max")) ts.max = std::strtoull(argv[++i], nullptr, 0);
    else if (arg("--dump-frames")) dump = argv[++i];
    else if (!std::strcmp(argv[i], "--direct")) direct = true;
    else if (!std::strcmp(argv[i], "--interp")) jit9 = jit7 = false;          // interpreter for both CPUs
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
  if (rom && direct) nds.setup_direct_boot();
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
  ts.pc_hist = std::getenv("TRACE_PC_HIST") != nullptr;
  const char* trace_start = std::getenv("TRACE_START_FRAME");   // suppress trace output before this frame
  const int trace_from = trace_start ? std::atoi(trace_start) : 0;
  for (int i = 0; i < frames; ++i) {
    if (trace && i == trace_from) {
      nds.trace = trace_cb; nds.trace_user = &ts;
#if DSPERATE_JIT
      ds::jit::set_trace(true);
#endif
    }
    nds.run_frame();
    if (dump_out) {   // raw 0xAARRGGBB, top screen then bottom, 256x192 each, one record per frame
      std::fwrite(nds.gpu.framebuffer(0), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
      std::fwrite(nds.gpu.framebuffer(1), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
    }
    if (per_frame && trace) { std::fprintf(stderr, "frame %d arm9 %llu arm7 %llu\n", i, ts.executed[0] - last9, ts.executed[1] - last7); last9 = ts.executed[0]; last7 = ts.executed[1]; }
  }
  if (dump_out) std::fclose(dump_out);
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
  std::fprintf(stderr, "ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
