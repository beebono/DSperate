// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// gles_probe DUMP [iterations] [out.ppm|out.raw]: vk_probe for the OpenGL ES
// raster (gl/gl_lean.h). Draws a recorded frame (headless --dump-gpu-frame,
// vk_dump.h) over and over, one at a time, and reports its cost, so a
// Vulkan-less device (the RG35XX SP's Mali-G31) can be measured without the
// emulator around it.
//
// Each frame is submitted onto an idle GPU and waited for, so the time from
// its flush to its fence signalling is its GPU time ("GPU (fence)"), which
// on the G31 blob is more reliable than timer queries (DS_GL_TIMING=1 adds
// those anyway). "submit+wait" is the whole round trip, including the
// collector's copy of the record out of its uncached PBO.
// Switches: DS_LEAN_SCALE=N (attachment scale), DS_LEAN_SKIP=hex (pass kinds
// left out, gl_lean.cpp), DS_PROBE_NOTEX=1, DS_PROBE_DISPCNT=hex.
#include "core/gpu/gl/gl_lean.h"
#include "core/gpu/vk/vk_dump.h"
#include "hwc_probe.h"
#include "probe_alpha.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ds;
using namespace ds::gpu::vk;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: gles_probe DUMP [iterations] [out.ppm|out.raw]\n"); return 2; }
  const int iters = argc > 2 ? std::atoi(argv[2]) : 200;
  const char* ppm = argc > 3 ? argv[3] : nullptr;

  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  GpuDumpHeader h;
  if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != GpuDumpHeader::kMagic) { std::fprintf(stderr, "not a gpu frame dump\n"); return 1; }
  if (h.version != 2) { std::fprintf(stderr, "dump version %u: regenerate it with this build's headless --dump-gpu-frame\n", h.version); return 1; }
  std::vector<GpuPoly> gp(h.npoly); std::vector<GpuVert> gv(h.nvert); std::vector<u32> tex(h.ntexels), shrun(static_cast<size_t>(h.npoly) * DS_SHRUN_LINES), rowpoly(h.nrows), spans(static_cast<size_t>(h.nspanrows) * 8);   // an older dump's row and span tables: read past
  auto rd = [&](void* p, size_t n) { return n == 0 || std::fread(p, n, 1, f) == 1; };
  if (!rd(gp.data(), sizeof(GpuPoly) * gp.size()) || !rd(gv.data(), sizeof(GpuVert) * gv.size()) || !rd(tex.data(), sizeof(u32) * tex.size()) ||
      !rd(shrun.data(), sizeof(u32) * shrun.size()) || !rd(rowpoly.data(), sizeof(u32) * rowpoly.size()) || !rd(spans.data(), sizeof(u32) * spans.size())) { std::fprintf(stderr, "short dump\n"); return 1; }
  std::fclose(f);
  if (const char* e = std::getenv("DS_PROBE_DISPCNT")) h.f.dispcnt = static_cast<u32>(std::strtoul(e, nullptr, 16));
  if (const char* e = std::getenv("DS_PROBE_NOTEX"); e && std::atoi(e)) for (auto& p : gp) p.flags &= ~(DS_PF_TEXTURED | DS_PF_TEX_ALPHA);
  for (auto& p : gp) { p.span_base = DS_SPAN_NONE; p.flags &= ~0x0C80u; }   // an older dump: no span rows, and the flags of the fill rule, the line case and the push
  probe::alpha_passes(gp, gv, tex);
  std::printf("frame %llu: %u polygons (%u opaque prefix), %u vertices, %u texel words, %u span rows, dispcnt %04x%s\n",
              (unsigned long long)h.frame, h.npoly, h.f.first_ordered, h.nvert, h.ntexels, h.nspanrows, h.f.dispcnt, (h.f.flags & DS_FF_WBUFFER) ? ", W-buffer" : "");

  std::string why;
  std::unique_ptr<gpu::gl::Lean> L = gpu::gl::Lean::create(false, &why);
  if (!L || !L->ready()) { std::fprintf(stderr, "GLES raster unavailable: %s\n", why.c_str()); return 1; }

  auto upload = [&]() {
    u32 cap = 0; u32* t = L->texel_buffer(&cap);
    std::memcpy(L->poly_buffer(), gp.data(), sizeof(GpuPoly) * gp.size());
    std::memcpy(L->vert_buffer(), gv.data(), sizeof(GpuVert) * gv.size());
    if (tex.size() > cap) { std::fprintf(stderr, "texel arena too small (%zu > %u)\n", tex.size(), cap); std::exit(1); }
    std::memcpy(t, tex.data(), sizeof(u32) * tex.size());
    *L->post_buffer() = h.post;
  };
  // The GL raster binds its context to the thread that submits: this one throughout.
  auto submit = [&]() { return L->submit(h.npoly, h.nvert, h.ntexels, h.f); };

  // Warm-up: program variants, first-use allocations (the G31 stalls 20-45 ms on a variant's first use).
  for (int i = 0; i < 3; ++i) { upload(); if (!submit()) { std::fprintf(stderr, "submit refused\n"); return 1; } L->wait(); }

  std::vector<double> ms, fence, timer;
  ms.reserve(iters); fence.reserve(iters); timer.reserve(iters);
  HwcProbe hwc;   // DS_PROBE_HWC=1: Mali counters over the timed loop
  hwc.begin();
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    const auto a = std::chrono::steady_clock::now();
    upload();
    if (!submit()) { std::fprintf(stderr, "submit refused at %d\n", i); return 1; }
    L->wait();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    if (L->fence_ns()) fence.push_back(static_cast<double>(L->fence_ns()) / 1e6);
    if (L->gpu_ns()) timer.push_back(static_cast<double>(L->gpu_ns()) / 1e6);
  }
  const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  hwc.end(iters);
  auto report = [](const char* what, std::vector<double>& v, const char* tail) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
    std::printf("%s median %.2f ms, p90 %.2f, max %.2f%s\n", what, q(0.5), q(0.9), v.back(), tail);
  };
  char tail[64]; std::snprintf(tail, sizeof tail, "; %u draws", L->draws());
  report("GPU (fence) per frame", fence, tail);
  report("GPU (timer query) per frame", timer, "");
  char tail2[64]; std::snprintf(tail2, sizeof tail2, " (%d frames, %.1f ms total)", iters, total);
  report("submit+wait per frame", ms, tail2);

  if (ppm) {
    const u32 S = L->scale();
    const u32* out = L->output();
    std::FILE* o = std::fopen(ppm, "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", ppm); return 1; }
    if (std::strlen(ppm) > 4 && !std::strcmp(ppm + std::strlen(ppm) - 4, ".raw")) {   // the record itself: RGB666 + 5-bit alpha words
      std::fwrite(out, sizeof(u32), 256 * 192 * S * S, o); std::fclose(o); return 0;
    }
    std::fprintf(o, "P6\n%u %u\n255\n", 256 * S, 192 * S);
    for (u32 i = 0; i < 256 * 192 * S * S; ++i) {
      const u32 c = out[i];
      const unsigned char px[3] = {static_cast<unsigned char>((c & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 8) & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 16) & 0x3F) * 255 / 63)};
      std::fwrite(px, 1, 3, o);
    }
    std::fclose(o);
    std::printf("wrote %s\n", ppm);
  }
  return 0;
}
