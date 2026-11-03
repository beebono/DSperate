// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The panel-density edge cut: the reference for the present shader
// (present.frag), which mirrors it. A 3D edge pixel's panel block is cut
// along the edge line instead of showing the 1x blend the DS makes of it:
// every panel pixel takes either the pixel's own unblended colour or the
// outside neighbour's, never a mix, so a silhouette steps at panel density
// (four steps per DS pixel at 4x) and keeps its corners. The line's position
// across the run's axis comes from the coverage; it tilts toward the
// position of the next edge pixel along the run, on the side the panel
// pixel is on, meeting it at the pixels' shared border.
//
// Input: the screen's frame (0xAARRGGBB, Gpu::framebuffer) and its edge
// plane (Gpu::edge_plane: 0xEERRGGBB, the edge byte over the pixel's own
// colour; Renderer3D::set_edge_export has the byte's layout). A word with
// bit 31 clear is no edge: zero, or a GPU frame's tag on a pixel beside an
// edge pixel, the only kind whose neighbours are worth looking up.
#pragma once
#include "core/types.h"

namespace ds::gpu {

// Position of the edge across the run's axis, in [0, 1] from the negative
// side (left / up): the polygon covers (cov + 1) / 32 of the pixel from its
// own side, the side away from the outside.
inline float edge_cut_pos(u32 e) {
  const float f = static_cast<float>((e & 0x1F) + 1) / 32.0f;
  return (e & 0x40) ? 1.0f - f : f;   // outside negative: the polygon is at the positive end
}

// Within one 6-bit step a channel (8-bit: 4): two polygons of one surface.
inline bool edge_cut_same(u32 a, u32 b) {
  for (int sh = 0; sh < 24; sh += 8) {
    const int d = static_cast<int>((a >> sh) & 255) - static_cast<int>((b >> sh) & 255);
    if (d < -4 || d > 4) return false;
  }
  return true;
}

// The edge line of edge byte `e` at source pixel (x, y), across the run's
// axis, at `along` (-0.5..0.5) along it: tilted toward the next edge pixel
// of the same kind on that side, else toward one whose outside faces back,
// beside it or one step across the axis toward the outside (a shallow line
// is held alternately by the polygon's pixel and the one just outside it:
// the same line, its position carried over by that step).
inline float edge_cut_line(const u32* edges, u32 e, int x, int y, float along) {
  const bool vert = e & 0x20, neg = e & 0x40;
  const int nx = x + (vert ? (along < 0 ? -1 : 1) : 0), ny = y + (vert ? 0 : (along < 0 ? -1 : 1));
  const float pos = edge_cut_pos(e);
  float pn = pos;
  if (nx >= 0 && nx < 256 && ny >= 0 && ny < 192) {
    const u32 n = edges[ny * 256 + nx] >> 24;
    if ((n & 0xE0) == (e & 0xE0)) pn = edge_cut_pos(n);
    else if ((n & 0xE0) == ((e ^ 0x40) & 0xE0)) pn = edge_cut_pos(n);   // the pixel beside it on the same row, the line's other side
    else {
      const int ox = nx + (vert ? 0 : (neg ? -1 : 1)), oy = ny + (vert ? (neg ? -1 : 1) : 0);
      if (ox >= 0 && ox < 256 && oy >= 0 && oy < 192) {
        const u32 o = edges[oy * 256 + ox] >> 24;
        if ((o & 0xE0) == ((e ^ 0x40) & 0xE0)) pn = edge_cut_pos(o) + (neg ? -1.0f : 1.0f);
      }
    }
  }
  return pos + (pn - pos) * (along < 0 ? -along : along);
}

inline u32 edge_cut_mix(u32 a, u32 b, float k) {   // a + (b - a) * k, per channel
  u32 r = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) {
    const float c = static_cast<float>((a >> sh) & 255) + (static_cast<float>((b >> sh) & 255) - static_cast<float>((a >> sh) & 255)) * k;
    r |= static_cast<u32>(c + 0.5f) << sh;
  }
  return r;
}
inline float edge_cut_clamp01(float k) { return k < 0 ? 0 : k > 1 ? 1 : k; }

// The colour of the panel point (u, v) in [0, 1)^2 inside source pixel
// (x, y). `bw` > 0 (the present's default, 1; DS_EDGE_BLEND): the two
// colours mixed by the point's distance from the line over a band `bw` DS
// pixels wide, reaching into the pixel beyond the edge pixel; 0: the hard cut.
inline u32 edge_cut_sample(const u32* frame, const u32* edges, int x, int y, float u, float v, float bw = 0) {
  const u32 s = frame[y * 256 + x];
  const u32 w = edges[y * 256 + x];
  if (!(w >> 31)) {   // no edge: 0, or the tag of a pixel beside an edge pixel (Gpu3D::edge_line)
    if (bw <= 0 || !w) return s;
    static const int nb[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    u32 c = s; float best = 0;
    for (const auto& d : nb) {
      const int qx = x + d[0], qy = y + d[1];
      if (qx < 0 || qx > 255 || qy < 0 || qy > 191) continue;
      const u32 wn = edges[qy * 256 + qx];
      if (!(wn >> 31)) continue;
      const u32 en = wn >> 24;
      const bool vn = en & 0x20, gn = en & 0x40;
      const bool toward = vn ? (d[0] == 0 && (d[1] == -1 ? !gn : gn)) : (d[1] == 0 && (d[0] == -1 ? !gn : gn));
      if (!toward) continue;
      const u32 own = 0xFF000000u | (wn & 0x00FFFFFFu);
      if (edge_cut_same(own, s)) continue;
      const float tn = vn ? v - static_cast<float>(d[1]) : u - static_cast<float>(d[0]);
      const float ln = edge_cut_line(edges, en, qx, qy, (vn ? u : v) - 0.5f);
      const float k = edge_cut_clamp01(0.5f + (gn ? tn - ln : ln - tn) / bw);
      if (k > best) { best = k; c = edge_cut_mix(s, own, k); }
    }
    return c;
  }
  const u32 e = w >> 24;
  const bool vert = e & 0x20, neg = e & 0x40;
  const float t = vert ? v : u;
  const float line = edge_cut_line(edges, e, x, y, (vert ? u : v) - 0.5f);
  const bool inside = neg ? t >= line : t < line;
  const u32 own = 0xFF000000u | (w & 0x00FFFFFFu);
  const int px = vert ? x : (neg ? x - 1 : x + 1), py = vert ? (neg ? y - 1 : y + 1) : y;
  if (px < 0 || px > 255 || py < 0 || py > 191) return own;
  const u32 o = frame[py * 256 + px];
  if (edge_cut_same(own, o)) return s;   // a seam inside one surface: nothing to cut
  // The outside pixel cut by another line of the same axis whose outside
  // faces back: two edges with a strip between them that owns neither pixel
  // (a horizon just above a hill's crest), so the DS drew it in neither; the
  // neighbour's colour is not the strip's, and the pixel stays whole.
  { const u32 n = edges[py * 256 + px] >> 24; if ((n & 0x80) && ((n ^ e) & 0x60) == 0x40) return s; }
  if (bw <= 0) return inside ? own : o;
  return edge_cut_mix(o, own, edge_cut_clamp01(0.5f + (neg ? t - line : line - t) / bw));
}

// A whole screen at n x: dst is (256 n) x (192 n), nearest outside the cut.
inline void edge_cut_screen(const u32* frame, const u32* edges, int n, u32* dst, float bw = 0) {
  for (int y = 0; y < 192; ++y)
    for (int fy = 0; fy < n; ++fy) {
      u32* const row = dst + static_cast<size_t>(y * n + fy) * 256 * n;
      for (int x = 0; x < 256; ++x)
        for (int fx = 0; fx < n; ++fx)
          row[x * n + fx] = edge_cut_sample(frame, edges, x, y, (fx + 0.5f) / n, (fy + 0.5f) / n, bw);
    }
}

} // namespace ds::gpu
