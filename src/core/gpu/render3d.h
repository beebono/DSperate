// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

class Gpu3D;
struct Polygon;
struct Vertex;
struct RenderState;
class VramMap;
struct VramView;

// Software rasteriser for the 3D engine's polygon list.
//
// Scanline renderer with the hardware's fixed-point edge stepping (18-bit
// slope fraction computed as x * (1/y)), its two-stage perspective-correct
// interpolation (a 9-bit factor along Y, 8-bit along X, then linear
// interpolation of the attributes by that factor), its edge fill rules, a
// two-deep pixel stack for anti-aliasing and the final pass (edge marking,
// fog, AA blend). The behaviour follows the documentation and software
// renderer of the melonDS project (GPLv3); output is checked against it
// pixel for pixel.
//
// Buffers are 258x194: a one-pixel border around the 256x192 frame so edge
// marking never tests outside the buffer.
class Renderer3D {
public:
  explicit Renderer3D(NDS& nds);
  void reset();

  // Rasterise the frame latched by the geometry engine.
  void render(const Gpu3D& gx);

  // One output line: RGB666 in bits 0-21, 5-bit alpha in bits 24-28.
  const u32* raw_line(u32 y) const { return &color_[FIRST + y * W]; }

  // Portable pixel-pipeline pieces, exposed for the unit tests.
  static u32 alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha);

private:
  NDS& nds_;
  static constexpr int W = 258, H = 194, SIZE = W * H, FIRST = W + 1;

  // Colour: R 0-5, G 8-13, B 16-21, A 24-28.
  // Attr: bits 0-3 edge flags (L/R/T/B), bit 4 back-facing, bits 8-12 AA
  // coverage, bit 15 fog, bits 16-21 translucent polygon id, bit 22
  // translucent, bits 24-29 opaque polygon id.
  // The second half of each buffer holds the pixel underneath (for AA).
  std::array<u32, SIZE * 2> color_{}, depth_{}, attr_{};
  std::array<u8, 512> stencil_{};
  bool prev_shadow_mask_ = false;

  // Perspective-correct interpolation factor between two endpoints.
  template <int dir> struct Interp {
    s32 x0 = 0, x1 = 0, xdiff = 0, x = 0;
    int shift = 0; bool linear = false, wbuffer = false;
    s32 xrecip_z = 0; s32 w0n = 0, w0d = 0, w1d = 0; u32 yfactor = 0;
    void setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf);
    void set_x(s32 xv);
    s32 interpolate(s32 y0, s32 y1) const;
    s32 interpolate_z(s32 z0, s32 z1) const;
  };

  // One polygon edge walked down the scanlines.
  template <int side> struct Slope {
    s32 increment = 0; bool negative = false, xmajor = false;
    Interp<1> interp;
    s32 x0 = 0, xmin = 0, xmax = 0, xlen = 0, ylen = 0, dx = 0, y = 0, xcov_incr = 0;
    s32 setup_dummy(s32 x0_, bool wbuf);
    s32 setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf);
    s32 step();
    s32 xval() const;
    template <bool swapped> void edge_params(s32* length, s32* coverage) const;
  };

  struct Edge {
    const Polygon* poly;
    Slope<0> left; Slope<1> right;
    s32 xl, xr;
    u32 cur_vl, cur_vr, next_vl, next_vr;
  };
  std::array<Edge, 2048> edges_{};

  const Gpu3D* gx_ = nullptr;
  const RenderState* rs_ = nullptr;
  const VramMap* vm_ = nullptr;
  const VramView* texv_ = nullptr;
  const VramView* palv_ = nullptr;

  u8  tex8(u32 addr) const;
  u16 tex16(u32 addr) const;
  u16 pal16(u32 addr) const;
  void texture_lookup(u32 texparam, u32 texpal, s16 s, s16 t, u16* color, u8* alpha) const;
  u32  shade_pixel(const Polygon& p, u8 vr, u8 vg, u8 vb, s16 s, s16 t) const;
  void plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow);
  void setup_left_edge(Edge& e, s32 y) const;
  void setup_right_edge(Edge& e, s32 y) const;
  void setup_polygon(Edge& e, const Polygon& p) const;
  void render_shadow_mask_line(Edge& e, s32 y);
  void render_polygon_line(Edge& e, s32 y);
  void render_line(s32 y, u32 npolys);
  u32  fog_density(u32 addr) const;
  void final_pass(s32 y);
  void clear_buffers();
};

} // namespace ds::gpu
