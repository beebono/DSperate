// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The geometry engine's per-frame input to the raster's vertex stage (the
// GPU raster's submit thread, Gpu3D::stream_list): what it takes to
// transform, classify, clip and emit the frame's polygons there, recorded as
// the commands run instead of the emulation thread building polygon RAM.
// The records are plain 4-byte fields.
//
// Every polygon is classified on its own: strip polygons name all their
// vertices, and the hardware's reuse of the previous strip polygon's vertex
// RAM entries is not modelled (it changed no polygon in the captures
// measured; it matters for vertex-RAM counts and a clipped-strip quirk).
#pragma once

#include "core/types.h"

#include <array>
#include <vector>

namespace ds::gpu {

struct GeomStream {
  struct Vert {
    u32 xy;      // object-space x | y << 16 (s16 each, 4.12)
    s32 z;       // object-space z, sign-extended
    u32 mtx;     // index into clip
    u32 color;   // vertex colour in force (lit or set), r | g << 8 | b << 16, 5 bits each
    u32 tex;     // texture coordinates after texgen, s | t << 16 (12.4, s16)
  };
  static_assert(sizeof(Vert) == 20, "GeomStream::Vert layout");
  struct Poly {
    u32 vtx[4];  // indices into verts
    u32 nverts;  // 3 or 4
    u32 attr;    // POLYGON_ATTR in force
    u32 texparam, texpal;
    u32 viewport;  // index into viewports
  };
  static_assert(sizeof(Poly) == 36, "GeomStream::Poly layout");

  std::vector<std::array<s32, 16>> clip;       // each clip matrix as rebuilt; entry 0 the one in force at the frame's start
  std::vector<Vert> verts;
  std::vector<Poly> polys;
  std::vector<std::array<u32, 6>> viewports;   // Gpu3D::viewport_, an entry per change between polygons
  u32 flush_attr = 0;                          // SWAP_BUFFERS mode in force (bit 1: W-buffer)
  u32 swap_attr = 0;                           // the SWAP_BUFFERS that closed the frame (bit 0: manual translucent sort)
  u32 zero_dot_w_limit = 0xFFFFFF;
  bool gpu_only = false;                       // the CPU built no polygon list for this frame: the GPU raster draws it from here

  // Writer state: which stream vertex each temp-vertex slot holds.
  u32 slot_vert[4] = {};

  // Start a frame; vectors keep their capacity.
  void begin(const std::array<s32, 16>& clip_now) {
    clip.assign(1, clip_now); verts.clear(); polys.clear(); viewports.clear();
  }
};

} // namespace ds::gpu
