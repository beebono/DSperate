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
// colour; Renderer3D::set_edge_export has the byte's layout).
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

// The colour of the panel point (u, v) in [0, 1)^2 inside source pixel (x, y).
// `np`: panel pixels per source pixel. Nonzero: DS_EDGE_BLEND's soft
// version, the two colours mixed by the panel point's distance from the line.
inline u32 edge_cut_sample(const u32* frame, const u32* edges, int x, int y, float u, float v, float np = 0) {
  const u32 w = edges[y * 256 + x];
  if (!w) return frame[y * 256 + x];
  const u32 e = w >> 24;
  const bool vert = e & 0x20, neg = e & 0x40;
  const float t = vert ? v : u, along = (vert ? u : v) - 0.5f;
  // The neighbour along the run on this side, if it is an edge pixel of the
  // same kind (axis and side), gives the line's tilt.
  const int nx = x + (vert ? (along < 0 ? -1 : 1) : 0), ny = y + (vert ? 0 : (along < 0 ? -1 : 1));
  const float pos = edge_cut_pos(e);
  float pn = pos;
  if (nx >= 0 && nx < 256 && ny >= 0 && ny < 192) {
    const u32 n = edges[ny * 256 + nx] >> 24;
    if ((n & 0xE0) == (e & 0xE0)) pn = edge_cut_pos(n);
  }
  const float line = pos + (pn - pos) * (along < 0 ? -along : along);
  const bool inside = neg ? t >= line : t < line;
  const u32 own = 0xFF000000u | (w & 0x00FFFFFFu);
  const int px = vert ? x : (neg ? x - 1 : x + 1), py = vert ? (neg ? y - 1 : y + 1) : y;
  if (px < 0 || px > 255 || py < 0 || py > 191) return own;
  const u32 o = frame[py * 256 + px];
  if (edge_cut_same(own, o)) return frame[y * 256 + x];   // a seam inside one surface: nothing to cut
  if (np <= 0) return inside ? own : o;
  float k = ((neg ? t - line : line - t)) * np + 0.5f;
  k = k < 0 ? 0 : k > 1 ? 1 : k;
  u32 r = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) {
    const float c = static_cast<float>((o >> sh) & 255) + (static_cast<float>((own >> sh) & 255) - static_cast<float>((o >> sh) & 255)) * k;
    r |= static_cast<u32>(c + 0.5f) << sh;
  }
  return r;
}

// A whole screen at n x: dst is (256 n) x (192 n), nearest outside the cut.
inline void edge_cut_screen(const u32* frame, const u32* edges, int n, u32* dst, bool soft = false) {
  for (int y = 0; y < 192; ++y)
    for (int fy = 0; fy < n; ++fy) {
      u32* const row = dst + static_cast<size_t>(y * n + fy) * 256 * n;
      for (int x = 0; x < 256; ++x)
        for (int fx = 0; fx < n; ++fx)
          row[x * n + fx] = edge_cut_sample(frame, edges, x, y, (fx + 0.5f) / n, (fy + 0.5f) / n, soft ? static_cast<float>(n) : 0.0f);
    }
}

} // namespace ds::gpu
