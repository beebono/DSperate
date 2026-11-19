// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Helpers for the raster probes.
#pragma once
#include "core/gpu/vk/vk_layout.h"
#include "core/gpu/alpha_grid.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
namespace probe {
// The pass flags of a dump's translucent polygons (DS_PF_A31_NONE / _ALL, as render3d.cpp sets them: a dump made
// before them has none).
inline void alpha_passes(std::vector<ds::gpu::vk::GpuPoly>& gp, const std::vector<ds::gpu::vk::GpuVert>& gv, const std::vector<ds::u32>& tex) {
  using namespace ds; using namespace ds::gpu::vk;
  const bool on = true;
  double area[4] = {}; u32 n[4] = {};
  for (GpuPoly& p : gp) {
    p.flags &= ~(DS_PF_A31_NONE | DS_PF_A31_ALL);
    if ((p.flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW | DS_PF_SHADOW_MASK)) != DS_PF_TRANSLUCENT) continue;
    int s0 = 0x7FFF, s1 = -0x8000, t0 = 0x7FFF, t1 = -0x8000;
    for (u32 k = 0; k < p.nverts; ++k) { const GpuVert& q = gv[p.first_vert + k]; s0 = std::min(s0, q.s); s1 = std::max(s1, q.s); t0 = std::min(t0, q.t); t1 = std::max(t1, q.t); }
    const bool textured = (p.flags & DS_PF_TEXTURED) != 0u; const u32 fmt = (p.texparam >> 26) & 7u;
    u64 g31 = ~u64{0}, gpart = 0;
    if (textured && (fmt == 1 || fmt == 6)) { g31 = ds::gpu::alpha_grid_of(tex.data() + p.tex_offset, p.tex_w, p.tex_h, 31, 31); gpart = ds::gpu::alpha_grid_of(tex.data() + p.tex_offset, p.tex_w, p.tex_h, 1, 30); }
    const u32 f = ds::gpu::alpha_pass_flags(g31, gpart, p.attr, p.texparam, textured, p.tex_w, p.tex_h, s0, s1, t0, t1, true);
    s64 a2 = 0; for (u32 k = 0; k < p.nverts; ++k) { const GpuVert& u = gv[p.first_vert + k]; const GpuVert& v = gv[p.first_vert + (k + 1) % p.nverts]; a2 += s64{u.fx} * v.fy - s64{v.fx} * u.fy; }
    area[f >> 8] += static_cast<double>(a2 < 0 ? -a2 : a2) / 512.0; ++n[f >> 8];
    if (on) p.flags |= f;
  }
  std::printf("alpha passes%s: translucent polygons (area /px) both %u (%.2f), no alpha-31 pass %u (%.2f), alpha-31 pass alone %u (%.2f), neither %u (%.2f)\n", on ? "" : " (off)",
              n[0], area[0] / 49152.0, n[1], area[1] / 49152.0, n[2], area[2] / 49152.0, n[3], area[3] / 49152.0);
}
}
