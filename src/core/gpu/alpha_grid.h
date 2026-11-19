// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Where a decoded texture is transparent, coarsely: the texture as 8x8
// blocks, one bit a block (bit ty * 8 + tx) set when the block holds a texel
// of alpha 0. The GPU raster asks it whether a polygon can fail the alpha
// test at all: one whose texcoord range reaches no such block cannot, and
// draws with the early depth test instead of the discard (B6 of
// GPU-RENDER-PLAN: half of Mario Kart DS's cut-out textured area, all of
// Spirit Tracks', only uses the opaque part of a texture that has holes
// elsewhere).
#include "core/types.h"
#include <cstddef>

namespace ds::gpu {

// The grid of `texels` (width * height words, alpha in bits 16-20). DS
// texture sides are 8 << n, so a block is side / 8 texels.
// With `lo` and `hi`: the blocks that hold a texel of alpha lo to hi instead.
inline u64 alpha_grid_of(const u32* texels, u32 width, u32 height, u32 lo = 0, u32 hi = 0) {
  const auto in = [lo, hi](u32 t) { const u32 a = (t >> 16) & 0x1Fu; return a >= lo && a <= hi; };
  if (width < 8 || height < 8) {   // not a DS size: any hole marks everything
    for (u32 i = 0; i < width * height; ++i) if (in(texels[i])) return ~u64{0};
    return 0;
  }
  const u32 bw = width / 8, bh = height / 8;
  u64 grid = 0;
  for (u32 y = 0; y < height; ++y) {
    const u32* row = texels + static_cast<size_t>(y) * width;
    u32 cols = 0;
    for (u32 x = 0; x < width; ++x) if (in(row[x])) cols |= 1u << (x / bw);
    grid |= u64{cols} << (8 * (y / bh));
  }
  return grid;
}

// The blocks of one axis (bits 0-7) that the texel range [lo, hi] samples
// under TEXIMAGE_PARAM's wrap: clamped, repeated, or repeated with every
// other period mirrored (ds_shade.glsl sample_tex).
inline u32 alpha_grid_axis(s32 lo, s32 hi, u32 side, bool repeat, bool flip) {
  if (side < 8 || hi < lo) return 0xFFu;
  const s32 n = static_cast<s32>(side), bs = n / 8;
  if (!repeat) { lo = lo < -1 ? -1 : lo; hi = hi > n ? n : hi; }
  else if (hi - lo + 1 >= 2 * n) return 0xFFu;
  u32 m = 0;
  for (s32 s = lo;; ) {
    s32 v;
    if (repeat) v = (flip && (s & n)) ? (n - 1) - (s & (n - 1)) : (s & (n - 1));
    else v = s < 0 ? 0 : s > n - 1 ? n - 1 : s;
    m |= 1u << (v / bs);
    if (s == hi) break;
    const s32 next = (s & ~(bs - 1)) + bs;   // the next block's first texel (two's complement: also below zero)
    s = next > hi ? hi : next;
  }
  return m;
}

// Whether any block of the columns x rows product is set.
inline bool alpha_grid_hit(u64 grid, u32 cols, u32 rows) {
  for (u32 r = 0; r < 8; ++r) if (((rows >> r) & 1u) && ((grid >> (8 * r)) & cols & 0xFFu)) return true;
  return false;
}

// Whether a polygon of this texture can fail the alpha test: texcoords in
// 12.4 as the vertices hold them (the fragment stage clamps a pixel's to the
// polygon's range, lean.frag). Decal mode takes the polygon's alpha, never the texel's.
inline bool alpha_grid_tested(u64 grid, u32 attr, u32 texparam, u32 width, u32 height, s32 s_lo, s32 s_hi, s32 t_lo, s32 t_hi) {
  if (!grid) return false;
  if (((attr >> 4) & 3u) == 1u) return false;
  return alpha_grid_hit(grid, alpha_grid_axis(s_lo >> 4, s_hi >> 4, width, (texparam & 0x10000u) != 0, (texparam & 0x40000u) != 0),
                              alpha_grid_axis(t_lo >> 4, t_hi >> 4, height, (texparam & 0x20000u) != 0, (texparam & 0x80000u) != 0));
}

// Which of a translucent polygon's two passes can draw nothing (DS_PF_A31_NONE 0x100: no pixel of alpha 31;
// DS_PF_A31_ALL 0x200: no pixel of alpha 1 to 30; vk_layout.h), exactly as ds_shade.glsl makes a pixel's
// alpha: the polygon's own where there is no texture or it is a decal, else
// ((texel + 1) * (polygon + 1) - 1) >> 5, which is 31 only for both at 31. `grid31` and `grid_part`: the
// texture's blocks with a texel of alpha 31, and of 1 to 30. `ranged`: the texcoords stay in the polygon's
// range (alpha_grid_tested); else the whole texture counts. A wireframe polygon (alpha 0) gets neither.
inline u32 alpha_pass_flags(u64 grid31, u64 grid_part, u32 attr, u32 texparam, bool textured, u32 width, u32 height,
                            s32 s_lo, s32 s_hi, s32 t_lo, s32 t_hi, bool ranged) {
  const u32 pa = (attr >> 16) & 0x1Fu;
  if (pa == 0u) return 0u;
  if (pa < 31u) return 0x100u;
  if (!textured || ((attr >> 4) & 1u) != 0u) return 0x200u;
  u32 cols = 0xFFu, rows = 0xFFu;
  if (ranged) {
    cols = alpha_grid_axis(s_lo >> 4, s_hi >> 4, width, (texparam & 0x10000u) != 0, (texparam & 0x40000u) != 0);
    rows = alpha_grid_axis(t_lo >> 4, t_hi >> 4, height, (texparam & 0x20000u) != 0, (texparam & 0x80000u) != 0);
  }
  return (alpha_grid_hit(grid31, cols, rows) ? 0u : 0x100u) | (alpha_grid_hit(grid_part, cols, rows) ? 0u : 0x200u);
}

} // namespace ds::gpu
