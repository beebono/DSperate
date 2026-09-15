// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Raster replay benchmark: the 3D rasteriser alone, on real frames, on the
// device. A save state carries the render list, the render registers and the
// texture VRAM, so re-running Gpu3D::render_frame on a loaded state rasterises
// exactly the frame the game drew -- with no CPU, DMA, 2D or pacing around it.
// Built for the target and run there, it measures what instruction counts
// cannot: time on the core (the A7 is stall-bound, ~2 cycles an instruction).
//
//   dsperate-raster-bench --bios9 B9 --bios7 B7 --firmware FW --rom ROM
//                         [--iters N] [--warm N] [--profile OUT] state.dss [state.dss ...]
//
// Each state: load, render --warm times (texture cache and page-ins), then
// time --iters renders, each followed by sync_all so a threaded raster is
// timed to completion. The output hash of every timed render must equal the
// first one; a mismatch is reported and fails the run, so the tool is also an
// on-device exactness check between builds (NEON / scalar / new kernels):
// equal hashes across builds mean equal pictures. DS_R3D_THREADS=0 gives the
// single-threaded cost of the kernels; unset, the shape the build would use.
//
// --profile OUT: a sampling profile of the timed renders (core/pc_sampler.h),
// for a device with no perf; tools/pc_profile.py folds it into functions,
// inlined ones included, with addr2line on a -g build of the same code. Every
// thread is sampled, so DS_R3D_THREADS=0 keeps the kernels on one thread.
#include "core/nds.h"
#include "core/state/state.h"
#include "core/host_cores.h"
#include "core/pc_sampler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<ds::u8> slurp(const char* path) {
  std::vector<ds::u8> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) { v.resize(static_cast<size_t>(n)); if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); }
  std::fclose(f);
  return v;
}

ds::u64 frame_hash(ds::NDS& nds) {
  const ds::gpu::Renderer3D::FrameRef f = nds.gpu3d.frame_ref();
  ds::u64 h = 1469598103934665603ull;
  for (ds::u32 y = 0; y < 192; ++y) {
    const ds::u32* l = f.line(y);
    for (ds::u32 x = 0; x < 256; ++x) h = (h ^ l[x]) * 1099511628211ull;
  }
  return h;
}

double render_once(ds::NDS& nds) {
  nds.gpu3d.force_full_raster();
  const auto t0 = std::chrono::steady_clock::now();
  nds.gpu3d.render_frame();
  nds.gpu3d.sync_raster();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
  const char* bios9 = "", *bios7 = "", *fw = "", *rom = nullptr, *profile = nullptr;
  int iters = 50, warm = 5;
  std::vector<const char*> states;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--rom")) rom = argv[++i];
    else if (arg("--iters")) iters = std::atoi(argv[++i]);
    else if (arg("--warm")) warm = std::atoi(argv[++i]);
    else if (arg("--profile")) profile = argv[++i];
    else if (argv[i][0] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    else states.push_back(argv[i]);
  }
  if (!rom || states.empty() || iters < 1) {
    std::fprintf(stderr, "usage: %s --bios9 B9 --bios7 B7 --firmware FW --rom ROM [--iters N] [--warm N] state.dss...\n", argv[0]);
    return 2;
  }
  std::fprintf(stderr, "raster-bench: %u cores, DS_R3D_THREADS=%s, %d iters, %d warm, build %s\n", ds::host_cores(),
               std::getenv("DS_R3D_THREADS") ? std::getenv("DS_R3D_THREADS") : "(default)", iters, warm,
#if DSPERATE_NEON
               "NEON"
#else
               "scalar"
#endif
  );
  bool ok = true;
  if (profile && !ds::pcsample::start()) { std::fprintf(stderr, "--profile: no sampler on this platform\n"); return 2; }
  for (const char* path : states) {
    ds::NDS nds;
    std::string err;
    ds::bios::UserSettings user;
    if (!nds.load_bios(bios9, bios7, fw, user, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
    if (!nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
    std::vector<ds::u8> bytes = slurp(path);
    ds::state::Reader r(bytes.data(), bytes.size());
    if (bytes.empty() || !nds.load_state(r, err)) { std::fprintf(stderr, "%s: cannot load: %s\n", path, bytes.empty() ? "unreadable" : err.c_str()); ok = false; continue; }
    const ds::u32 polys = nds.gpu3d.render_polygon_count();
    for (int w = 0; w < warm; ++w) render_once(nds);
    const ds::u64 want = (render_once(nds), frame_hash(nds));
    std::vector<double> ms;
    ms.reserve(static_cast<size_t>(iters));
    int bad = 0;
    for (int k = 0; k < iters; ++k) {
      if (profile) ds::pcsample::set_active(true);
      ms.push_back(render_once(nds));
      if (profile) ds::pcsample::set_active(false);
      if (frame_hash(nds) != want) ++bad;
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("%-40s polys %4u  min %7.3f  median %7.3f  mean %7.3f ms  hash %016llx%s\n", path, polys, sorted.front(),
                sorted[sorted.size() / 2], sum / static_cast<double>(ms.size()), static_cast<unsigned long long>(want),
                bad ? "  HASH MISMATCH" : "");
    if (bad) { std::fprintf(stderr, "%s: %d of %d renders differed from the first\n", path, bad, iters); ok = false; }
  }
  if (profile && !ds::pcsample::write(profile)) { std::fprintf(stderr, "cannot write %s\n", profile); ok = false; }
  return ok ? 0 : 1;
}
