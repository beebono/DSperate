// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless CLI: boots the BIOS/firmware (and optionally a ROM), runs N frames,
// optionally writing per-CPU instruction traces in the shared trace format:
//   <pc> <instr> <cpsr> r0 .. r14     (hex, one line per instruction)
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
};

void trace_cb(ds::CpuContext& cpu, ds::u32 instr, void* user) {
  auto* t = static_cast<TraceState*>(user);
  const int i = static_cast<int>(cpu.which);
  t->executed[i]++;
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
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr, *trace = nullptr;
  int frames = 60; bool direct = false;
  TraceState ts;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--frames")) frames = std::atoi(argv[++i]);
    else if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--trace")) trace = argv[++i];
    else if (arg("--max")) ts.max = std::strtoull(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--direct")) direct = true;
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
  if (trace) {
    ts.out[0] = std::fopen((std::string(trace) + ".arm9.trace").c_str(), "w");
    ts.out[1] = std::fopen((std::string(trace) + ".arm7.trace").c_str(), "w");
    nds.trace = trace_cb; nds.trace_user = &ts;
  }
  const char* per_frame = std::getenv("TRACE_PER_FRAME");
  unsigned long long last9 = 0, last7 = 0;
  for (int i = 0; i < frames; ++i) {
    nds.run_frame();
    if (per_frame && trace) { std::fprintf(stderr, "frame %d arm9 %llu arm7 %llu\n", i, ts.executed[0] - last9, ts.executed[1] - last7); last9 = ts.executed[0]; last7 = ts.executed[1]; }
  }
  if (trace) { std::fclose(ts.out[0]); std::fclose(ts.out[1]);
    std::fprintf(stderr, "arm9: %llu lines (%llu instrs), arm7: %llu lines (%llu instrs), cap %llu lines each\n",
                 ts.count[0], ts.executed[0], ts.count[1], ts.executed[1], ts.max); }
  std::fprintf(stderr, "ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
