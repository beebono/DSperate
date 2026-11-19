// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// How the lean rasters (vk_lean.cpp, gl/gl_lean.cpp) split a translucent run
// into draws.
#include "core/gpu/vk/vk_layout.h"

namespace ds::gpu::vk {

// A translucent run's first pass draws its alpha-31 pixels as opaque
// (lean.frag DS_ALPHA_SEL 1, which discards every other pixel). Only a
// polygon whose alpha (POLYGON_ATTR bits 16-20) is 31 can produce one:
// decal and untextured pixels take the polygon alpha as is, and modulate's
// ((texel alpha + 1) * (polygon alpha + 1) - 1) >> 5 tops out at 30 below
// it. So the pass is drawn only over the run's alpha-31 polygons, and its
// output is unchanged (B0 of GPU-RENDER-PLAN: NSMB has none, SM64DS about
// half of its translucent area).
//
// Writes the alpha-31 sub-ranges of [first, end) to lo/count and returns how
// many there are: 0 skips the pass. Past kA31Ranges, the whole run is drawn
// once instead, where the extra draws would cost more than the fragments saved.
constexpr u32 kA31Ranges = 8;
// A polygon flagged DS_PF_A31_NONE has none either (its texture has no texel of alpha 31 in reach).
// `all_only`: the polygons whose every pixel is at 31 alone (the blended pass keeps the others' pixels of alpha 31).
inline u32 alpha_31_ranges(const GpuPoly* polys, u32 first, u32 end, u32* lo, u32* count, bool all_only = false) {
  const auto has = [all_only](const GpuPoly& p) { return ((p.attr >> 16) & 0x1Fu) == 31u && (p.flags & DS_PF_A31_NONE) == 0u && (!all_only || (p.flags & DS_PF_A31_ALL) != 0u); };
  u32 n = 0;
  for (u32 k = first; k < end;) {
    if (!has(polys[k])) { ++k; continue; }
    u32 e = k + 1;
    while (e < end && has(polys[e])) ++e;
    if (n == kA31Ranges) { lo[0] = first; count[0] = end - first; return 1; }
    lo[n] = k; count[n] = e - k; ++n;
    k = e;
  }
  return n;
}

// The run's second pass, the pixels of alpha 1 to 30: over the polygons that can have one (not DS_PF_A31_ALL),
// in the run's order. As alpha_31_ranges; 0 skips the pass.
inline u32 alpha_part_ranges(const GpuPoly* polys, u32 first, u32 end, u32* lo, u32* count) {
  u32 n = 0;
  for (u32 k = first; k < end;) {
    if ((polys[k].flags & DS_PF_A31_ALL) != 0u) { ++k; continue; }
    u32 e = k + 1;
    while (e < end && (polys[e].flags & DS_PF_A31_ALL) == 0u) ++e;
    if (n == kA31Ranges) { lo[0] = first; count[0] = end - first; return 1; }
    lo[n] = k; count[n] = e - k; ++n;
    k = e;
  }
  return n;
}

// A draw's vertex count (B7 of GPU-RENDER-PLAN). A polygon is a fan of
// nverts - 2 triangles and the vertex stages cull the triangles past it, so
// a draw over [first, first + count) needs only its largest polygon's fan.
// 24 vertices an instance (a 10-gon's) cost the Malis the vertex work of
// eight triangles per polygon in every pass; nearly all runs are triangles
// and quads (6).
// `centre`: polygons of four or more vertices are fans around their centre (DS_FF2_CENTRE), a triangle a vertex.
inline u32 fan_vertices(const GpuPoly* polys, u32 first, u32 count, bool centre = false) {
  u32 m = 3;
  for (u32 k = first; k < first + count; ++k) m = polys[k].nverts > m ? polys[k].nverts : m;
  m = m < 10 ? m : 10;
  return 3 * (centre && m >= 4 ? m : m - 2);
}

} // namespace ds::gpu::vk
