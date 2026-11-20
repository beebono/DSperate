// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS_GPU_TIMELINE=FILE (diagnostics): who has the GPU and when. The 3D raster, its vertex stage and the
// Vulkan present each write a line per event, "TAG cpu_ns a b id": the CPU's CLOCK_MONOTONIC at the call, and
// for the *_gpu lines the job's own first and last GPU timestamps in ns (the device's clock, not the CPU's: the
// reader finds the offset; id tells the present's instances apart). Read with tools/gpu_timeline.py beside DS_FRAME_SERIES.
// A stream frame's way to the raster, on the same clock: J_swap (a: the display line of the SWAP_BUFFERS), J_post (the
// render at line 215 posts the frame), J_start and J_list (a: polygons) on the submit thread, then the raster's R_submit.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
namespace ds::gpu::tl {
inline std::FILE* file() { static std::FILE* const f = [] { const char* p = std::getenv("DS_GPU_TIMELINE"); return p && *p ? std::fopen(p, "w") : nullptr; }(); return f; }
inline bool on() { return file() != nullptr; }
inline unsigned long long now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return static_cast<unsigned long long>(ts.tv_sec) * 1000000000ull + static_cast<unsigned long long>(ts.tv_nsec); }
inline void ev(const char* tag, unsigned long long a = 0, unsigned long long b = 0, unsigned id = 0) {
  std::FILE* const f = file();
  if (!f) return;
  static std::mutex m;
  const unsigned long long t = now();
  std::lock_guard<std::mutex> lk(m);
  std::fprintf(f, "%s %llu %llu %llu %u\n", tag, t, a, b, id);
}
}  // namespace ds::gpu::tl
