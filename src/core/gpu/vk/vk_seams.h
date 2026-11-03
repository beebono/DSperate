// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The edge cut's seams: an edge two band-eligible polygons share (a seam
// inside one surface, where a mesh's faces meet) against one only one has
// (a silhouette, or a face's edge against another surface). The band pass
// draws the seams first and the rest after, so where a seam meets a
// silhouette in one pixel the silhouette's byte is the one kept (SM64DS: a
// seam between two faces of a hill reaching the crest cut the sea pixel there
// across instead of along the crest). Marked per vertex in
// GpuVert::pad_: bit 0, the edge from that vertex to the polygon's next is
// shared; bit 1, shared by polygons of different DS polygon ids (POLYGON_ATTR
// bits 24-29: the id the hardware's edge marking outlines between, so games
// give objects their own), an object's outline against another rather than
// a seam within it; bits 2 up, the other polygon's index plus one, so the
// band pass can tell a pixel the seam really divides (owned by one of the
// two) from one of a third surface the seam only crosses in front of or
// beside (SM64DS: the backdrop's sky/sea horizon running into a hill cut the
// sky pixels above the crest with the hill's green). The band pass draws
// the three classes in turn: seams, id outlines, then silhouettes
// (lean_edge.vert).
#include "core/gpu/vk/vk_layout.h"
#include <cstdint>
#include <vector>

namespace ds::gpu::vk {

struct SeamTable {
  static constexpr u32 kBits = 16;   // 65536 slots for at most DS_MAX_VERTS edges
  std::vector<u64> key = std::vector<u64>(1u << kBits);
  std::vector<u32> vert = std::vector<u32>(1u << kBits), poly = std::vector<u32>(1u << kBits), gen = std::vector<u32>(1u << kBits);
  u32 g = 0;
};

inline void mark_shared_edges(const GpuPoly* polys, u32 npoly, GpuVert* verts, SeamTable& t) {
  const u32 mask = (1u << SeamTable::kBits) - 1;
  if (++t.g == 0) { std::fill(t.gen.begin(), t.gen.end(), 0u); t.g = 1; }
  for (u32 k = 0; k < npoly; ++k) {
    const GpuPoly& p = polys[k];
    if (p.nverts < 3 || (p.flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW | DS_PF_SHADOW_MASK)) || p.xmax == p.xmin || p.ybot == p.ytop) continue;   // as lean_edge.vert skips
    for (u32 e = 0; e < p.nverts; ++e) {
      const u32 ia = p.first_vert + e, ib = p.first_vert + (e + 1 == p.nverts ? 0 : e + 1);
      const u64 a = (static_cast<u64>(static_cast<u16>(verts[ia].fx)) << 16) | static_cast<u16>(verts[ia].fy);
      const u64 b = (static_cast<u64>(static_cast<u16>(verts[ib].fx)) << 16) | static_cast<u16>(verts[ib].fy);
      if (a == b) continue;
      const u64 key = a < b ? (a << 32) | b : (b << 32) | a;
      u32 h = static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> (64 - SeamTable::kBits));
      for (;;) {
        if (t.gen[h] != t.g) { t.gen[h] = t.g; t.key[h] = key; t.vert[h] = ia; t.poly[h] = k; break; }
        if (t.key[h] == key) {
          const u32 m = ((polys[t.poly[h]].attr >> 24) & 0x3Fu) != ((p.attr >> 24) & 0x3Fu) ? 3u : 1u;
          verts[t.vert[h]].pad_ = (verts[t.vert[h]].pad_ & 3u) | m | ((k + 1) << 2); verts[ia].pad_ = (verts[ia].pad_ & 3u) | m | ((t.poly[h] + 1) << 2); break;
        }
        h = (h + 1) & mask;
      }
    }
  }
}

} // namespace ds::gpu::vk
