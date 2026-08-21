// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/render3d.h"
#include "core/gpu/gpu3d.h"
#include "core/gpu/vram_map.h"
#include "core/nds.h"
#include "core/profile.h"

#include <algorithm>
#include <cstring>

namespace ds::gpu {

// ---- interpolation --------------------------------------------------------------
//
// Attributes are not divided per pixel. A perspective-correct factor between
// the two endpoints is computed (9 fractional bits along Y, 8 along X) and the
// attribute is then interpolated linearly by that factor. When both W values
// are equal (with low bits clear) the factor is skipped and the interpolation
// is plainly linear, which is what 2D-style polygons want.

template <int dir>
void Renderer3D::Interp<dir>::setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf) {
  x0 = x0_; x1 = x1_; xdiff = x1_ - x0_; wbuffer = wbuf;
  xrecip_z = xdiff != 0 ? (1 << 22) / xdiff : 0;
  const u32 mask = dir ? 0x7E : 0x7F;
  linear = (w0 == w1) && !(w0 & mask) && !(w1 & mask);
  if (dir) { w0n = w0 >> 1; w0d = (w0 + ((w0 & ~w1) & 1)) >> 1; w1d = w1 >> 1; shift = 9; }
  else { w0n = w0; w0d = w0; w1d = w1; shift = 8; }
}

template <int dir>
void Renderer3D::Interp<dir>::set_x(s32 xv) {
  xv -= x0;
  x = xv;
  if (xdiff != 0 && (!linear || wbuffer)) {
    const u32 num = static_cast<u32>(xv * w0n) << shift;
    const u32 den = static_cast<u32>(xv * w0d) + static_cast<u32>((xdiff - xv) * w1d);
    yfactor = den == 0 ? 0 : num / den;
  }
}

template <int dir>
s32 Renderer3D::Interp<dir>::interpolate(s32 y0, s32 y1) const {
  if (xdiff == 0 || y0 == y1) return y0;
  if (!linear) {
    if (y0 < y1) return y0 + static_cast<s32>((static_cast<s64>(y1 - y0) * yfactor) >> shift);
    return y1 + static_cast<s32>((static_cast<s64>(y0 - y1) * ((1 << shift) - yfactor)) >> shift);
  }
  if (y0 < y1) return y0 + static_cast<s32>(static_cast<s64>(y1 - y0) * x / xdiff);
  return y1 + static_cast<s32>(static_cast<s64>(y0 - y1) * (xdiff - x) / xdiff);
}

template <int dir>
s32 Renderer3D::Interp<dir>::interpolate_z(s32 z0, s32 z1) const {
  if (xdiff == 0 || z0 == z1) return z0;
  if (wbuffer) {
    if (z0 < z1) return z0 + static_cast<s32>((static_cast<s64>(z1 - z0) * yfactor) >> shift);
    return z1 + static_cast<s32>((static_cast<s64>(z0 - z1) * ((1 << shift) - yfactor)) >> shift);
  }
  // Z-buffering interpolates linearly with a reciprocal of the span.
  s32 base, disp, factor;
  if (z0 < z1) { base = z0; disp = z1 - z0; factor = x; }
  else { base = z1; disp = z0 - z1; factor = xdiff - x; }
  if (dir) {
    int sh = 0;
    while (disp > 0x3FF) { disp >>= 1; ++sh; }
    return base + static_cast<s32>(((static_cast<s64>(disp) * factor * xrecip_z) >> 22) << sh);
  }
  disp >>= 9;
  return base + static_cast<s32>((static_cast<s64>(disp) * factor * xrecip_z) >> 13);
}

// ---- edges ------------------------------------------------------------------------

template <int side>
s32 Renderer3D::Slope<side>::setup_dummy(s32 x0_, bool wbuf) {
  dx = 0; x0 = x0_; xmin = x0_; xmax = x0_;
  increment = 0; xmajor = false;
  interp.setup(0, 0, 0, 0, wbuf);
  interp.set_x(0);
  xcov_incr = 0;
  return x0_;
}

// The slope has an 18-bit fraction and is computed as (x1-x0) * (1/ylen),
// not as a direct division, which is what the hardware does.
template <int side>
s32 Renderer3D::Slope<side>::setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf) {
  x0 = x0_; y = y_;
  if (x1_ > x0_) { xmin = x0_; xmax = x1_ - 1; negative = false; }
  else if (x1_ < x0_) { xmin = x1_; xmax = x0_ - 1; negative = true; }
  else { xmin = x0_; xmax = xmin; negative = false; }
  xlen = xmax + 1 - xmin;
  ylen = y1 - y0;
  if (ylen == 0) increment = 0;
  else if (ylen == xlen && xlen != 1) increment = 0x40000;
  else {
    const s32 yrecip = (1 << 18) / ylen;
    increment = (x1_ - x0_) * yrecip;
    if (increment < 0) increment = -increment;
  }
  xmajor = increment > 0x40000;
  if (side) {
    if (xmajor) dx = negative ? (0x20000 + 0x40000) : (increment - 0x20000);
    else if (increment != 0) dx = negative ? 0x40000 : 0;
    else dx = 0;
  } else {
    if (xmajor) dx = negative ? ((increment - 0x20000) + 0x40000) : 0x20000;
    else if (increment != 0) dx = negative ? 0x40000 : 0;
    else dx = 0;
  }
  dx += (y_ - y0) * increment;
  const s32 x = xval();
  const int interpoffset = (increment >= 0x40000) && (side ^ static_cast<int>(negative));
  interp.setup(y0 - interpoffset, y1 - interpoffset, w0, w1, wbuf);
  interp.set_x(y_);
  if (xmajor) xcov_incr = (ylen << 10) / xlen;
  return x;
}

template <int side>
s32 Renderer3D::Slope<side>::step() {
  dx += increment;
  ++y;
  const s32 x = xval();
  interp.set_x(y);
  return x;
}

template <int side>
s32 Renderer3D::Slope<side>::xval() const {
  s32 r = negative ? x0 - (dx >> 18) : x0 + (dx >> 18);
  if (r < xmin) r = xmin; else if (r > xmax) r = xmax;
  return r;
}

// Length of the edge's run on this scanline and its anti-aliasing coverage.
// X-major edges return the first pixel's coverage plus a per-pixel increment
// (flagged by bit 31); Y-major edges a single 5-bit coverage.
template <int side>
template <bool swapped>
void Renderer3D::Slope<side>::edge_params(s32* length, s32* coverage) const {
  if (xmajor) {
    if (!swapped || side) {
      if (side ^ static_cast<int>(negative)) *length = (dx >> 18) - ((dx - increment) >> 18);
      else *length = ((dx + increment) >> 18) - (dx >> 18);
    }
    s32 startx = dx >> 18;
    if (negative) startx = xlen - startx;
    if (side) startx = startx - *length + 1;
    const s32 startcov = (((startx << 10) + 0x1FF) * ylen) / xlen;
    *coverage = static_cast<s32>(0x80000000u) | ((startcov & 0x3FF) << 12) | (xcov_incr & 0x3FF);
    if (swapped) *length = 1;
    return;
  }
  *length = 1;
  if (increment == 0) { *coverage = swapped ? 0 : 31; return; }
  s32 cov = ((dx >> 9) + (increment >> 10)) >> 4;
  if ((cov >> 5) != (dx >> 18)) cov = 31;
  cov &= 0x1F;
  if (swapped) { if (side ^ static_cast<int>(negative)) cov = 0x1F - cov; }
  else { if (!(side ^ static_cast<int>(negative))) cov = 0x1F - cov; }
  *coverage = cov;
}

// ---- pixel pipeline ---------------------------------------------------------------

Renderer3D::Renderer3D(NDS& nds) : nds_(nds) { reset(); }

void Renderer3D::reset() {
  color_.fill(0); depth_.fill(0); attr_.fill(0);
  stencil_.fill(0);
  prev_shadow_mask_ = false;
}

u8 Renderer3D::tex8(u32 addr) const {
  addr &= texv_->addr_mask();
  const u8* p = texv_->ptr[addr / VramView::BLOCK];
  return p ? p[addr & (VramView::BLOCK - 1)] : vm_->read8(*texv_, addr);
}
u16 Renderer3D::tex16(u32 addr) const {
  addr &= texv_->addr_mask();
  const u8* p = texv_->ptr[addr / VramView::BLOCK];
  if (p) { u16 v; std::memcpy(&v, p + (addr & (VramView::BLOCK - 1)), 2); return v; }
  return vm_->read16(*texv_, addr);
}
u16 Renderer3D::pal16(u32 addr) const {
  addr &= palv_->addr_mask();
  const u8* p = palv_->ptr[addr / VramView::BLOCK];
  if (p) { u16 v; std::memcpy(&v, p + (addr & (VramView::BLOCK - 1)), 2); return v; }
  return vm_->read16(*palv_, addr);
}

void Renderer3D::texture_lookup(u32 texparam, u32 texpal, s16 s, s16 t, u16* color, u8* alpha) const {
  u32 addr = (texparam & 0xFFFF) << 3;
  const s32 width = 8 << ((texparam >> 20) & 7), height = 8 << ((texparam >> 23) & 7);
  s >>= 4; t >>= 4;
  // Wrap / clamp / flip.
  if (texparam & (1 << 16)) {
    if (texparam & (1 << 18)) { if (s & width) s = static_cast<s16>((width - 1) - (s & (width - 1))); else s = static_cast<s16>(s & (width - 1)); }
    else s = static_cast<s16>(s & (width - 1));
  } else { if (s < 0) s = 0; else if (s >= width) s = static_cast<s16>(width - 1); }
  if (texparam & (1 << 17)) {
    if (texparam & (1 << 19)) { if (t & height) t = static_cast<s16>((height - 1) - (t & (height - 1))); else t = static_cast<s16>(t & (height - 1)); }
    else t = static_cast<s16>(t & (height - 1));
  } else { if (t < 0) t = 0; else if (t >= height) t = static_cast<s16>(height - 1); }

  const u8 alpha0 = (texparam & (1 << 29)) ? 0 : 31;
  switch ((texparam >> 26) & 7) {
  case 1: {   // A3I5
    const u8 px = tex8(addr + (t * width + s));
    *color = pal16((texpal << 4) + ((px & 0x1F) << 1));
    *alpha = static_cast<u8>(((px >> 3) & 0x1C) + (px >> 6));
    break;
  }
  case 2: {   // 4 colours
    u8 px = tex8(addr + ((t * width + s) >> 2));
    px = (px >> ((s & 3) << 1)) & 3;
    *color = pal16((texpal << 3) + (px << 1));
    *alpha = px == 0 ? alpha0 : 31;
    break;
  }
  case 3: {   // 16 colours
    u8 px = tex8(addr + ((t * width + s) >> 1));
    px = (s & 1) ? (px >> 4) : (px & 0xF);
    *color = pal16((texpal << 4) + (px << 1));
    *alpha = px == 0 ? alpha0 : 31;
    break;
  }
  case 4: {   // 256 colours
    const u8 px = tex8(addr + (t * width + s));
    *color = pal16((texpal << 4) + (px << 1));
    *alpha = px == 0 ? alpha0 : 31;
    break;
  }
  case 5: {   // 4x4 compressed
    addr += ((t & 0x3FC) * (width >> 2)) + (s & 0x3FC) + (t & 3);
    addr &= 0x7FFFF;
    u32 slot1 = 0x20000 + ((addr & 0x1FFFC) >> 1);
    if (addr >= 0x40000) slot1 += 0x10000;
    u8 val;
    if (addr >= 0x20000 && addr < 0x40000) val = 0;      // texels can't live in slot 1
    else val = tex8(addr) >> (2 * (s & 3));
    const u16 palinfo = tex16(slot1);
    const u32 paloff = (palinfo & 0x3FFF) << 2;
    const u32 base = (texpal << 4) + paloff;
    auto mix = [&](u32 ma, u32 mb, u32 sh) -> u16 {
      const u16 c0 = pal16(base), c1 = pal16(base + 2);
      const u32 r = ((c0 & 0x1F) * ma + (c1 & 0x1F) * mb) >> sh;
      const u32 g = (((c0 & 0x3E0) * ma + (c1 & 0x3E0) * mb) >> sh) & 0x3E0;
      const u32 b = (((c0 & 0x7C00) * ma + (c1 & 0x7C00) * mb) >> sh) & 0x7C00;
      return static_cast<u16>(r | g | b);
    };
    switch (val & 3) {
    case 0: *color = pal16(base); *alpha = 31; break;
    case 1: *color = pal16(base + 2); *alpha = 31; break;
    case 2:
      if ((palinfo >> 14) == 1) *color = mix(1, 1, 1);
      else if ((palinfo >> 14) == 3) *color = mix(5, 3, 3);
      else *color = pal16(base + 4);
      *alpha = 31;
      break;
    default:
      if ((palinfo >> 14) == 2) { *color = pal16(base + 6); *alpha = 31; }
      else if ((palinfo >> 14) == 3) { *color = mix(3, 5, 3); *alpha = 31; }
      else { *color = 0; *alpha = 0; }
      break;
    }
    break;
  }
  case 6: {   // A5I3
    const u8 px = tex8(addr + (t * width + s));
    *color = pal16((texpal << 4) + ((px & 7) << 1));
    *alpha = px >> 3;
    break;
  }
  case 7: {   // direct colour
    *color = tex16(addr + ((t * width + s) << 1));
    *alpha = (*color & 0x8000) ? 31 : 0;
    break;
  }
  default: *color = 0; *alpha = 0; break;
  }
}

namespace {
inline u32 c15_to_18(u16 c, u32 shift) { u32 v = (shift == 0 ? (c << 1) : (c >> shift)) & 0x3E; if (v) ++v; return v; }
inline void rgb15_to_666(u16 c, u32& r, u32& g, u32& b) { r = c15_to_18(c, 0); g = c15_to_18(c, 4); b = c15_to_18(c, 9); }
}

// Depth test: 'less than' normally; 'less or equal' for a front-facing pixel
// over an opaque back-facing one, or within a tolerance when the polygon
// asks for equal-depth testing (±0x200 Z-buffered, ±0xFF W-buffered).
namespace {
inline bool depth_equal_z(s32 dstz, s32 z, u32) { return static_cast<u32>((dstz - z) + 0x200) <= 0x400; }
inline bool depth_equal_w(s32 dstz, s32 z, u32) { return static_cast<u32>((dstz - z) + 0xFF) <= 0x1FE; }
inline bool depth_less(s32 dstz, s32 z, u32) { return z < dstz; }
inline bool depth_less_front(s32 dstz, s32 z, u32 dstattr) { return (dstattr & 0x00400010) == 0x00000010 ? z <= dstz : z < dstz; }
using DepthFn = bool (*)(s32, s32, u32);
DepthFn pick_depth_test(const Polygon& p) {
  if (p.attr & (1 << 14)) return p.wbuffer ? depth_equal_w : depth_equal_z;
  return p.facing ? depth_less_front : depth_less;
}
}

u32 Renderer3D::alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha) {
  u32 dsta = dst >> 24;
  if (dsta == 0) return src;
  u32 r = src & 0x3F, g = (src >> 8) & 0x3F, b = (src >> 16) & 0x3F;
  if (dispcnt & (1 << 3)) {
    const u32 a1 = alpha + 1;
    r = ((r * a1) + ((dst & 0x3F) * (32 - a1))) >> 5;
    g = ((g * a1) + (((dst >> 8) & 0x3F) * (32 - a1))) >> 5;
    b = ((b * a1) + (((dst >> 16) & 0x3F) * (32 - a1))) >> 5;
  }
  if (alpha > dsta) dsta = alpha;
  return r | (g << 8) | (b << 16) | (dsta << 24);
}

u32 Renderer3D::shade_pixel(const Polygon& p, u8 vr, u8 vg, u8 vb, s16 s, s16 t) const {
  u32 r, g, b, a;
  const u32 blendmode = (p.attr >> 4) & 3;
  const u32 polyalpha = (p.attr >> 16) & 0x1F;
  const bool wireframe = polyalpha == 0;
  const bool highlight = rs_->dispcnt & (1 << 1);
  if (blendmode == 2) {
    if (highlight) { vg = vr; vb = vr; }        // highlight: all components from red, toon colour added later
    else { u32 tr, tg, tb; rgb15_to_666(rs_->toon[vr >> 1], tr, tg, tb); vr = tr; vg = tg; vb = tb; }
  }
  if ((rs_->dispcnt & 1) && ((p.texparam >> 26) & 7) != 0) {
    u16 tcolor; u8 talpha;
    texture_lookup(p.texparam, p.texpal, s, t, &tcolor, &talpha);
    u32 tr, tg, tb; rgb15_to_666(tcolor, tr, tg, tb);
    if (blendmode & 1) {   // decal
      if (talpha == 0) { r = vr; g = vg; b = vb; }
      else if (talpha == 31) { r = tr; g = tg; b = tb; }
      else {
        r = ((tr * talpha) + (vr * (31 - talpha))) >> 5;
        g = ((tg * talpha) + (vg * (31 - talpha))) >> 5;
        b = ((tb * talpha) + (vb * (31 - talpha))) >> 5;
      }
      a = polyalpha;
    } else {               // modulate
      r = ((tr + 1) * (vr + 1) - 1) >> 6;
      g = ((tg + 1) * (vg + 1) - 1) >> 6;
      b = ((tb + 1) * (vb + 1) - 1) >> 6;
      a = ((talpha + 1) * (polyalpha + 1) - 1) >> 5;
    }
  } else { r = vr; g = vg; b = vb; a = polyalpha; }
  if (blendmode == 2 && highlight) {
    u32 tr, tg, tb; rgb15_to_666(rs_->toon[vr >> 1], tr, tg, tb);
    r += tr; g += tg; b += tb;
    if (r > 63) r = 63;
    if (g > 63) g = 63;
    if (b > 63) b = 63;
  }
  if (wireframe) a = 31;
  return r | (g << 8) | (b << 16) | (a << 24);
}

void Renderer3D::plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow) {
  const u32 dstattr = attr_[addr];
  u32 attr = (polyattr & 0xE0F0) | ((polyattr >> 8) & 0xFF0000) | (1u << 22) | (dstattr & 0xFF001F0F);
  if (shadow) {
    // Shadows skip pixels of their own polygon id, opaque or translucent.
    if (dstattr & (1u << 22)) { if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return; }
    else if ((dstattr & 0x3F000000) == (polyattr & 0x3F000000)) return;
  } else if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return;   // equal translucent ids don't blend
  if (!(dstattr & (1u << 15))) attr &= ~(1u << 15);
  color = alpha_blend(rs_->dispcnt, color, color_[addr], color >> 24);
  if (z != 0xFFFFFFFFu) depth_[addr] = z;
  color_[addr] = color;
  attr_[addr] = attr;
}

// ---- polygon setup ------------------------------------------------------------------

void Renderer3D::setup_left_edge(Edge& e, s32 y) const {
  const Polygon& p = *e.poly;
  while (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) {
    e.cur_vl = e.next_vl;
    if (p.facing) { e.next_vl = e.cur_vl + 1; if (e.next_vl >= p.nverts) e.next_vl = 0; }
    else { e.next_vl = e.cur_vl - 1; if (static_cast<s32>(e.next_vl) < 0) e.next_vl = p.nverts - 1; }
  }
  const Vertex &a = gx_->vertex(p.vtx[e.cur_vl]), &b = gx_->vertex(p.vtx[e.next_vl]);
  e.xl = e.left.setup(a.sx, b.sx, a.sy, b.sy, p.w[e.cur_vl], p.w[e.next_vl], y, p.wbuffer);
}

void Renderer3D::setup_right_edge(Edge& e, s32 y) const {
  const Polygon& p = *e.poly;
  while (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) {
    e.cur_vr = e.next_vr;
    if (p.facing) { e.next_vr = e.cur_vr - 1; if (static_cast<s32>(e.next_vr) < 0) e.next_vr = p.nverts - 1; }
    else { e.next_vr = e.cur_vr + 1; if (e.next_vr >= p.nverts) e.next_vr = 0; }
  }
  const Vertex &a = gx_->vertex(p.vtx[e.cur_vr]), &b = gx_->vertex(p.vtx[e.next_vr]);
  e.xr = e.right.setup(a.sx, b.sx, a.sy, b.sy, p.w[e.cur_vr], p.w[e.next_vr], y, p.wbuffer);
}

void Renderer3D::setup_polygon(Edge& e, const Polygon& p) const {
  const u32 n = p.nverts;
  u32 vtop = p.vtop, vbot = p.vbot;
  e.poly = &p;
  e.cur_vl = vtop; e.cur_vr = vtop;
  if (p.facing) {
    e.next_vl = e.cur_vl + 1; if (e.next_vl >= n) e.next_vl = 0;
    e.next_vr = e.cur_vr - 1; if (static_cast<s32>(e.next_vr) < 0) e.next_vr = n - 1;
  } else {
    e.next_vl = e.cur_vl - 1; if (static_cast<s32>(e.next_vl) < 0) e.next_vl = n - 1;
    e.next_vr = e.cur_vr + 1; if (e.next_vr >= n) e.next_vr = 0;
  }
  if (p.ybot == p.ytop) {
    // Flat polygon: a single span from the leftmost to the rightmost vertex.
    vtop = 0; vbot = 0;
    for (u32 i : {1u, n - 1}) {
      if (gx_->vertex(p.vtx[i]).sx < gx_->vertex(p.vtx[vtop]).sx) vtop = i;
      if (gx_->vertex(p.vtx[i]).sx > gx_->vertex(p.vtx[vbot]).sx) vbot = i;
    }
    e.cur_vl = vtop; e.next_vl = vtop; e.cur_vr = vbot; e.next_vr = vbot;
    e.xl = e.left.setup_dummy(gx_->vertex(p.vtx[e.cur_vl]).sx, p.wbuffer);
    e.xr = e.right.setup_dummy(gx_->vertex(p.vtx[e.cur_vr]).sx, p.wbuffer);
  } else {
    setup_left_edge(e, p.ytop);
    setup_right_edge(e, p.ytop);
  }
}

// ---- scanline rendering ------------------------------------------------------------

namespace {
// Per-scanline span description shared by the shadow-mask and colour passes.
struct Span {
  s32 xstart, xend;
  bool l_fill, r_fill;
  s32 l_len, r_len, l_cov, r_cov;
  s32 wl, wr, zl, zr;
  bool swapped;
};
}

void Renderer3D::render_shadow_mask_line(Edge& e, s32 y) {
  const Polygon& p = *e.poly;
  u32 polyalpha = (p.attr >> 16) & 0x1F;
  const bool wireframe = polyalpha == 0;
  const DepthFn depth_test = pick_depth_test(p);

  if (!prev_shadow_mask_) std::memset(&stencil_[256 * (y & 1)], 0, 256);
  prev_shadow_mask_ = true;

  if (p.ytop != p.ybot) {
    if (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) setup_left_edge(e, y);
    if (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) setup_right_edge(e, y);
  }
  s32 xstart = e.xl, xend = e.xr;
  s32 wl = e.left.interp.interpolate(p.w[e.cur_vl], p.w[e.next_vl]);
  s32 wr = e.right.interp.interpolate(p.w[e.cur_vr], p.w[e.next_vr]);
  s32 zl = e.left.interp.interpolate_z(p.z[e.cur_vl], p.z[e.next_vl]);
  s32 zr = e.right.interp.interpolate_z(p.z[e.cur_vr], p.z[e.next_vr]);
  if (e.right.increment == 0 && (e.left.increment != 0 || xstart != xend) && xend != 0) --xend;

  bool l_fill, r_fill; s32 l_len, r_len, l_cov, r_cov;
  const bool always_fill = (rs_->dispcnt & ((1 << 4) | (1 << 5))) || (polyalpha < 31 && (rs_->dispcnt & (1 << 3))) || wireframe;
  if (xstart > xend) {
    const Vertex &vlnext = gx_->vertex(p.vtx[e.next_vr]), &vrnext = gx_->vertex(p.vtx[e.next_vl]);
    e.right.edge_params<true>(&l_len, &l_cov);
    e.left.edge_params<true>(&r_len, &r_cov);
    std::swap(xstart, xend); std::swap(wl, wr); std::swap(zl, zr);
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = (e.right.negative || !e.right.xmajor) || ((y == p.ybot - 1) && e.right.xmajor && (vlnext.sx != vrnext.sx));
      r_fill = (!e.left.negative && e.left.xmajor) || (!(e.left.negative && e.left.xmajor) && e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.left.xmajor && (vlnext.sx != vrnext.sx));
    }
  } else {
    const Vertex &vlnext = gx_->vertex(p.vtx[e.next_vl]), &vrnext = gx_->vertex(p.vtx[e.next_vr]);
    e.left.edge_params<false>(&l_len, &l_cov);
    e.right.edge_params<false>(&r_len, &r_cov);
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = ((e.left.negative || !e.left.xmajor) || ((y == p.ybot - 1) && e.left.xmajor && (vlnext.sx != vrnext.sx))) ||
               ((e.left.increment == e.right.increment) && (xstart + l_len == xend + 1));
      r_fill = (!e.right.negative && e.right.xmajor) || (e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.right.xmajor && (vlnext.sx != vrnext.sx));
    }
  }

  if (wireframe) polyalpha = 31;
  if (polyalpha <= rs_->alpha_ref) return;

  int yedge = 0;
  if (y == p.ytop) yedge = 0x4; else if (y == p.ybot - 1) yedge = 0x8;
  s32 x = xstart;
  Interp<0> ix; ix.setup(xstart, xend + 1, wl, wr, p.wbuffer);
  if (x < 0) x = 0;

  // Set stencil bits where the depth test fails; draw nothing.
  auto stencil_span = [&](s32 xlimit) {
    for (; x < xlimit; ++x) {
      u32 addr = FIRST + y * W + x;
      ix.set_x(x);
      const s32 z = ix.interpolate_z(zl, zr);
      const u32 dstattr = attr_[addr];
      if (!depth_test(static_cast<s32>(depth_[addr]), z, dstattr)) stencil_[256 * (y & 1) + x] = 1;
      if (dstattr & 0xF) {
        addr += SIZE;
        if (!depth_test(static_cast<s32>(depth_[addr]), z, attr_[addr])) stencil_[256 * (y & 1) + x] |= 2;
      }
    }
  };
  s32 xlimit = std::min({xstart + l_len, xend + 1, 256});
  if (!l_fill) x = xlimit; else stencil_span(xlimit);
  xlimit = std::min({xend - r_len + 1, xend + 1, 256});
  if (wireframe && !yedge) x = std::max(x, xlimit); else stencil_span(xlimit);
  xlimit = std::min(xend + 1, 256);
  if (r_fill) stencil_span(xlimit);

  e.xl = e.left.step();
  e.xr = e.right.step();
}

void Renderer3D::render_polygon_line(Edge& e, s32 y) {
  const Polygon& p = *e.poly;
  u32 polyattr = p.attr & 0x3F008000;
  if (!p.facing) polyattr |= (1 << 4);
  const u32 polyalpha = (p.attr >> 16) & 0x1F;
  const bool wireframe = polyalpha == 0;
  const DepthFn depth_test = pick_depth_test(p);
  const u32 dispcnt = rs_->dispcnt;
  prev_shadow_mask_ = false;

  if (p.ytop != p.ybot) {
    if (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) setup_left_edge(e, y);
    if (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) setup_right_edge(e, y);
  }
  s32 xstart = e.xl, xend = e.xr;
  s32 wl = e.left.interp.interpolate(p.w[e.cur_vl], p.w[e.next_vl]);
  s32 wr = e.right.interp.interpolate(p.w[e.cur_vr], p.w[e.next_vr]);
  s32 zl = e.left.interp.interpolate_z(p.z[e.cur_vl], p.z[e.next_vl]);
  s32 zr = e.right.interp.interpolate_z(p.z[e.cur_vr], p.z[e.next_vr]);
  // Right vertical edges are pushed one pixel left unless the span is a
  // single pixel at the screen's left edge.
  if (e.right.increment == 0 && (e.left.increment != 0 || xstart != xend) && xend != 0) --xend;

  const Vertex *vlcur, *vlnext, *vrcur, *vrnext;
  const Interp<1>* istart; const Interp<1>* iend;
  bool l_fill, r_fill; s32 l_len, r_len, l_cov, r_cov;
  const bool always_fill = (dispcnt & ((1 << 4) | (1 << 5))) || (polyalpha < 31 && (dispcnt & (1 << 3))) || wireframe;
  if (xstart > xend) {
    // Swapped edges: the hardware walks them backwards, which breaks the
    // X-major edge lengths (and the AA on them) in a specific way.
    vlcur = &gx_->vertex(p.vtx[e.cur_vr]); vlnext = &gx_->vertex(p.vtx[e.next_vr]);
    vrcur = &gx_->vertex(p.vtx[e.cur_vl]); vrnext = &gx_->vertex(p.vtx[e.next_vl]);
    istart = &e.right.interp; iend = &e.left.interp;
    e.right.edge_params<true>(&l_len, &l_cov);
    e.left.edge_params<true>(&r_len, &r_cov);
    std::swap(xstart, xend); std::swap(wl, wr); std::swap(zl, zr);
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = (e.right.negative || !e.right.xmajor) || ((y == p.ybot - 1) && e.right.xmajor && (vlnext->sx != vrnext->sx));
      r_fill = (!e.left.negative && e.left.xmajor) || (!(e.left.negative && e.left.xmajor) && e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.left.xmajor && (vlnext->sx != vrnext->sx));
    }
  } else {
    vlcur = &gx_->vertex(p.vtx[e.cur_vl]); vlnext = &gx_->vertex(p.vtx[e.next_vl]);
    vrcur = &gx_->vertex(p.vtx[e.cur_vr]); vrnext = &gx_->vertex(p.vtx[e.next_vr]);
    istart = &e.left.interp; iend = &e.right.interp;
    e.left.edge_params<false>(&l_len, &l_cov);
    e.right.edge_params<false>(&r_len, &r_cov);
    // Fill rules for opaque edges: left edges fill when their slope is <= 1,
    // right edges when > 1 or vertical; the bottom pixel of a negative
    // X-major edge fills next to a flat bottom; fully overlapping identical
    // edges fill. AA, edge marking, blended translucency or wireframe fill all.
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = ((e.left.negative || !e.left.xmajor) || ((y == p.ybot - 1) && e.left.xmajor && (vlnext->sx != vrnext->sx))) ||
               ((e.left.increment == e.right.increment) && (xstart + l_len == xend + 1));
      r_fill = (!e.right.negative && e.right.xmajor) || (e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.right.xmajor && (vlnext->sx != vrnext->sx));
    }
  }

  // Attributes at both ends of the span.
  const s32 rl = istart->interpolate(vlcur->fcol[0], vlnext->fcol[0]), gl = istart->interpolate(vlcur->fcol[1], vlnext->fcol[1]), bl = istart->interpolate(vlcur->fcol[2], vlnext->fcol[2]);
  const s32 sl = istart->interpolate(vlcur->tex[0], vlnext->tex[0]), tl = istart->interpolate(vlcur->tex[1], vlnext->tex[1]);
  const s32 rr = iend->interpolate(vrcur->fcol[0], vrnext->fcol[0]), gr = iend->interpolate(vrcur->fcol[1], vrnext->fcol[1]), br = iend->interpolate(vrcur->fcol[2], vrnext->fcol[2]);
  const s32 sr = iend->interpolate(vrcur->tex[0], vrnext->tex[0]), tr = iend->interpolate(vrcur->tex[1], vrnext->tex[1]);

  int yedge = 0;
  if (y == p.ytop) yedge = 0x4; else if (y == p.ybot - 1) yedge = 0x8;
  s32 x = xstart;
  Interp<0> ix; ix.setup(xstart, xend + 1, wl, wr, p.wbuffer);
  if (x < 0) x = 0;
  s32 xcov = 0;
  const bool aa = dispcnt & (1 << 4);

  // part: 0 = left edge, 1 = inside, 2 = right edge
  auto draw_span = [&](s32 xlimit, int part, int edge) {
    for (; x < xlimit; ++x) {
      u32 addr = FIRST + y * W + x;
      u32 dstattr = attr_[addr];
      if (p.shadow) {
        const u8 st = stencil_[256 * (y & 1) + x];
        if (!st) continue;
        if (!(st & 1)) addr += SIZE;
        if (!(st & 2)) dstattr &= ~0xFu;      // no shadow under anti-aliased edges
      }
      ix.set_x(x);
      const s32 z = ix.interpolate_z(zl, zr);
      // Failing against the top pixel, try the one underneath.
      if (!depth_test(static_cast<s32>(depth_[addr]), z, dstattr)) {
        if (!(dstattr & 0xF) || addr >= static_cast<u32>(SIZE)) continue;
        addr += SIZE;
        dstattr = attr_[addr];
        if (!depth_test(static_cast<s32>(depth_[addr]), z, dstattr)) continue;
      }
      const u32 vr = ix.interpolate(rl, rr), vg = ix.interpolate(gl, gr), vb = ix.interpolate(bl, br);
      const s16 s = static_cast<s16>(ix.interpolate(sl, sr)), t = static_cast<s16>(ix.interpolate(tl, tr));
      const u32 color = shade_pixel(p, vr >> 3, vg >> 3, vb >> 3, s, t);
      const u8 alpha = color >> 24;
      if (alpha <= rs_->alpha_ref) continue;
      if (alpha == 31) {
        u32 attr = polyattr | edge;
        bool push = false;
        if (aa) {
          if (part == 1) { if (attr & 0xF) { attr |= (0x1F << 8); push = true; } }
          else {
            s32 cov = part == 0 ? l_cov : r_cov;
            if (cov & static_cast<s32>(0x80000000u)) {
              if (part == 0) { cov = xcov >> 5; if (cov > 31) cov = 31; xcov += (l_cov & 0x3FF); }
              else { cov = 0x1F - (xcov >> 5); if (cov < 0) cov = 0; xcov += (r_cov & 0x3FF); }
            }
            attr |= (cov << 8);
            push = true;
          }
        }
        if (push && addr < static_cast<u32>(SIZE)) {
          color_[addr + SIZE] = color_[addr]; depth_[addr + SIZE] = depth_[addr]; attr_[addr + SIZE] = attr_[addr];
        }
        depth_[addr] = z; color_[addr] = color; attr_[addr] = attr;
      } else {
        const u32 zz = (p.attr & (1 << 11)) ? static_cast<u32>(z) : 0xFFFFFFFFu;
        plot_translucent(addr, color, zz, polyattr, p.shadow);
        if ((dstattr & 0xF) && addr < static_cast<u32>(SIZE)) plot_translucent(addr + SIZE, color, zz, polyattr, p.shadow);
      }
    }
  };

  s32 xlimit = std::min({xstart + l_len, xend + 1, 256});
  if (l_cov & static_cast<s32>(0x80000000u)) { xcov = (l_cov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
  if (!l_fill) x = xlimit; else draw_span(xlimit, 0, yedge | 0x1);
  xlimit = std::min({xend - r_len + 1, xend + 1, 256});
  if (wireframe && !yedge) x = std::max(x, xlimit); else draw_span(xlimit, 1, yedge);
  xlimit = std::min(xend + 1, 256);
  if (r_cov & static_cast<s32>(0x80000000u)) { xcov = (r_cov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
  if (r_fill) draw_span(xlimit, 2, yedge | 0x2);

  e.xl = e.left.step();
  e.xr = e.right.step();
}

void Renderer3D::render_line(s32 y, u32 npolys) {
  for (u32 i = 0; i < npolys; ++i) {
    Edge& e = edges_[i];
    const Polygon& p = *e.poly;
    if (y >= p.ytop && (y < p.ybot || (y == p.ytop && p.ybot == p.ytop))) {
      if (p.shadow_mask) render_shadow_mask_line(e, y);
      else render_polygon_line(e, y);
    }
  }
}

// ---- final pass -----------------------------------------------------------------------

u32 Renderer3D::fog_density(u32 addr) const {
  u32 z = depth_[addr];
  u32 id, frac;
  if (z < rs_->fog_offset) { id = 0; frac = 0; }
  else {
    // (Z - offset) >> 2 << shift: bits 0-16 fraction, 17+ table index. The
    // value can wrap in 32 bits for large shifts, as on hardware.
    z -= rs_->fog_offset;
    z = (z >> 2) << rs_->fog_shift;
    id = z >> 17;
    if (id >= 32) { id = 32; frac = 0; } else frac = z & 0x1FFFF;
  }
  u32 d = ((rs_->fog_density[id] * (0x20000 - frac)) + (rs_->fog_density[id + 1] * frac)) >> 17;
  if (d >= 127) d = 128;
  return d;
}

void Renderer3D::final_pass(s32 y) {
  const u32 dispcnt = rs_->dispcnt;
  if (dispcnt & (1 << 5)) {
    // Edge marking on the topmost pixels, against the four neighbours.
    for (int x = 0; x < 256; ++x) {
      const u32 addr = FIRST + y * W + x;
      const u32 attr = attr_[addr];
      if (!(attr & 0xF)) continue;
      const u32 id = attr >> 24;
      const u32 z = depth_[addr];
      if ((id != (attr_[addr - 1] >> 24) && z < depth_[addr - 1]) || (id != (attr_[addr + 1] >> 24) && z < depth_[addr + 1]) ||
          (id != (attr_[addr - W] >> 24) && z < depth_[addr - W]) || (id != (attr_[addr + W] >> 24) && z < depth_[addr + W])) {
        u32 r, g, b; rgb15_to_666(rs_->edge[id >> 3], r, g, b);
        color_[addr] = r | (g << 8) | (b << 16) | (color_[addr] & 0xFF000000);
        attr_[addr] = (attr & 0xFFFFE0FF) | 0x00001000;    // coverage broken for the AA pass
      }
    }
  }
  if (dispcnt & (1 << 7)) {
    // Fog on the top two pixels (the lower one feeds anti-aliasing).
    const bool fogcolor = !(dispcnt & (1 << 6));
    u32 fr, fg, fb; rgb15_to_666(static_cast<u16>(rs_->fog_color), fr, fg, fb);
    const u32 fa = (rs_->fog_color >> 16) & 0x1F;
    auto apply = [&](u32 addr) {
      const u32 d = fog_density(addr);
      const u32 c = color_[addr];
      u32 r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F, a = (c >> 24) & 0x1F;
      if (fogcolor) {
        r = ((fr * d) + (r * (128 - d))) >> 7;
        g = ((fg * d) + (g * (128 - d))) >> 7;
        b = ((fb * d) + (b * (128 - d))) >> 7;
      }
      a = ((fa * d) + (a * (128 - d))) >> 7;
      color_[addr] = r | (g << 8) | (b << 16) | (a << 24);
    };
    for (int x = 0; x < 256; ++x) {
      u32 addr = FIRST + y * W + x;
      const u32 attr = attr_[addr];
      if (attr & (1 << 15)) apply(addr);
      if (!(attr & 0xF)) continue;
      addr += SIZE;
      if (attr_[addr] & (1 << 15)) apply(addr);
    }
  }
  if (dispcnt & (1 << 4)) {
    // Anti-aliasing: blend edge pixels with the pixel underneath by coverage.
    for (int x = 0; x < 256; ++x) {
      const u32 addr = FIRST + y * W + x;
      const u32 attr = attr_[addr];
      if (!(attr & 0xF)) continue;
      u32 cov = (attr >> 8) & 0x1F;
      if (cov == 0x1F) continue;
      if (cov == 0) { color_[addr] = color_[addr + SIZE]; continue; }
      const u32 top = color_[addr], bot = color_[addr + SIZE];
      u32 tr = top & 0x3F, tg = (top >> 8) & 0x3F, tb = (top >> 16) & 0x3F, ta = (top >> 24) & 0x1F;
      const u32 br = bot & 0x3F, bg = (bot >> 8) & 0x3F, bb = (bot >> 16) & 0x3F, ba = (bot >> 24) & 0x1F;
      ++cov;
      if (ba > 0) {
        tr = ((tr * cov) + (br * (32 - cov))) >> 5;
        tg = ((tg * cov) + (bg * (32 - cov))) >> 5;
        tb = ((tb * cov) + (bb * (32 - cov))) >> 5;
      }
      ta = ((ta * cov) + (ba * (32 - cov))) >> 5;
      color_[addr] = tr | (tg << 8) | (tb << 16) | (ta << 24);
    }
  }
}

void Renderer3D::clear_buffers() {
  const u32 clearz = ((rs_->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
  u32 polyid = rs_->clear_attr1 & 0x3F000000;
  // Border for edge marking.
  for (int x = 0; x < W; ++x) { color_[x] = 0; depth_[x] = clearz; attr_[x] = polyid; }
  for (int x = W; x < W * 193; x += W) {
    color_[x] = 0; depth_[x] = clearz; attr_[x] = polyid;
    color_[x + 257] = 0; depth_[x + 257] = clearz; attr_[x + 257] = polyid;
  }
  for (int x = W * 193; x < W * 194; ++x) { color_[x] = 0; depth_[x] = clearz; attr_[x] = polyid; }

  if (rs_->dispcnt & (1 << 14)) {
    // Clear image from texture slots 2 (colour) and 3 (depth), scrolled.
    u8 yoff = (rs_->clear_attr2 >> 24) & 0xFF;
    for (int y = 0; y < W * 192; y += W) {
      u8 xoff = (rs_->clear_attr2 >> 16) & 0xFF;
      for (int x = 0; x < 256; ++x) {
        const u16 v2 = tex16(0x40000 + (yoff << 9) + (xoff << 1));
        const u16 v3 = tex16(0x60000 + (yoff << 9) + (xoff << 1));
        u32 r, g, b; rgb15_to_666(v2, r, g, b);
        const u32 a = (v2 & 0x8000) ? 0x1F000000 : 0;
        const u32 addr = FIRST + y + x;
        color_[addr] = r | (g << 8) | (b << 16) | a;
        depth_[addr] = ((v3 & 0x7FFF) * 0x200) + 0x1FF;
        attr_[addr] = polyid | (v3 & 0x8000);
        ++xoff;
      }
      ++yoff;
    }
  } else {
    u32 r, g, b; rgb15_to_666(static_cast<u16>(rs_->clear_attr1), r, g, b);
    const u32 a = (rs_->clear_attr1 >> 16) & 0x1F;
    const u32 color = r | (g << 8) | (b << 16) | (a << 24);
    polyid |= (rs_->clear_attr1 & 0x8000);
    for (int y = 0; y < W * 192; y += W)
      for (int x = 0; x < 256; ++x) { const u32 addr = FIRST + y + x; color_[addr] = color; depth_[addr] = clearz; attr_[addr] = polyid; }
  }
}

void Renderer3D::render(const Gpu3D& gx) {
  gx_ = &gx;
  rs_ = &gx.render_state();
  vm_ = &nds_.bus.vram_map();
  texv_ = &vm_->texture;
  palv_ = &vm_->texpal;
  { DS_PROF(R3D_CLEAR); clear_buffers(); }
  u32 n = 0;
  const Polygon* const* polys = gx.render_polygons();
  for (u32 i = 0; i < gx.render_polygon_count(); ++i) {
    if (polys[i]->degenerate) continue;
    setup_polygon(edges_[n++], *polys[i]);
  }
  { DS_PROF(R3D_SPANS); render_line(0, n); }
  for (s32 y = 1; y < 192; ++y) { { DS_PROF(R3D_SPANS); render_line(y, n); } { DS_PROF(R3D_FINAL); final_pass(y - 1); } }
  { DS_PROF(R3D_FINAL); final_pass(191); }
}

} // namespace ds::gpu
