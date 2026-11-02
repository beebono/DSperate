// The DS's own per-pixel attributes from the span table, for the draw
// (lean.frag) and the edge cut's cut-out pass (lean_cutout.frag), which
// must sample the texel the draw did. Needs `spans` (binding 4) and
// GpuPoly in scope.
#ifndef DS_SPANS_GLSL
#define DS_SPANS_GLSL
// Exact unsigned a / b for the quotients here (at most 2^17), as a float
// divide and one correction: Mali has no integer divider.
uint ds_udiv(uint a, uint b) {
  uint q = uint(float(a) / float(b));
  uint r = a - q * b;
  if (r >= b) ++q; else if (r > a) --q;   // r wrapped: q was one too many
  return q;
}
// The same with a reciprocal already in hand (the row's 1/xdiff, shared by
// five quotients): a product's rounding can be off by two, so two corrections.
uint ds_rdiv(uint n, uint d, float rcp) {
  uint q = uint(float(n) * rcp);
  uint r = n - q * d;
  if (r > n) { --q; r += d; if (r > n) --q; }
  else if (r >= d) { ++q; r -= d; if (r >= d) ++q; }
  return q;
}
int ds_persp(int y0, int y1, uint fac) {
  if (y0 == y1) return y0;
  if (y0 < y1) return y0 + int((uint(y1 - y0) * fac) >> 8);
  return y1 + int((uint(y0 - y1) * (256u - fac)) >> 8);
}
int ds_lin(int y0, int y1, int xv, int xdiff, float rcp) {
  if (y0 == y1) return y0;
  if (y0 < y1) return y0 + int(ds_rdiv(uint((y1 - y0) * xv), uint(xdiff), rcp));
  return y1 + int(ds_rdiv(uint((y0 - y1) * (xdiff - xv)), uint(xdiff), rcp));
}
// The DS's own attributes for this pixel: the row's span endpoints from the
// span table (vk_layout.h, the CPU raster's Slope/Interp output), then the
// hardware's fixed-point interpolation along the row (kernels_ref.cpp:
// span_factor, span_attr_persp, lin_at), so colour and texcoords equal the
// CPU raster's at every pixel the DS draws. A pixel the GPU's coverage
// reaches past the span reads the nearest endpoint. Equal endpoints (flat
// colour, most rows) and an untextured polygon's texcoords skip the
// arithmetic, which under MSAA runs for every edge fragment of this scene's
// many small polygons. False: no row (the fallback is the hardware's
// interpolation).
bool ds_attrs(GpuPoly p, bool textured, out ivec3 rgb, out ivec2 st) {
  if (p.span_base == DS_SPAN_NONE) return false;
  int y = int(gl_FragCoord.y), ry0 = max(p.ytop, 0), ry1 = min(p.ybot, 191);
  if (y < ry0 || y > ry1) return false;
  uint i = (p.span_base + uint(y - ry0)) * 2u;
  uvec4 a = spans[i], b = spans[i + 1u];
  int xstart = int(a.x), xend = int(a.y), wl = int(a.z), wr = int(a.w);
  ivec3 lrgb = ivec3(b.x & 0x3FFu, (b.x >> 10) & 0x3FFu, (b.x >> 20) & 0x3FFu);
  ivec2 lst = ivec2(int(b.y << 16) >> 16, int(b.y) >> 16);
  rgb = lrgb; st = lst;
  bool do_rgb = b.x != b.z, do_st = textured && b.y != b.w;
  if (!do_rgb && !do_st) return true;
  int xdiff = xend + 1 - xstart;
  if (xdiff <= 0) return true;
  int xv = clamp(int(gl_FragCoord.x) - xstart, 0, xdiff - 1);
  ivec3 rrgb = ivec3(b.z & 0x3FFu, (b.z >> 10) & 0x3FFu, (b.z >> 20) & 0x3FFu);
  ivec2 rst = ivec2(int(b.w << 16) >> 16, int(b.w) >> 16);
  if (wl == wr && (wl & 0x7F) == 0) {   // linear
    float rcp = 1.0 / float(xdiff);
    if (do_rgb) rgb = ivec3(ds_lin(lrgb.r, rrgb.r, xv, xdiff, rcp), ds_lin(lrgb.g, rrgb.g, xv, xdiff, rcp), ds_lin(lrgb.b, rrgb.b, xv, xdiff, rcp));
    if (do_st) st = ivec2(ds_lin(lst.x, rst.x, xv, xdiff, rcp), ds_lin(lst.y, rst.y, xv, xdiff, rcp));
    return true;
  }
  uint fac;
  if (wl == wr) fac = (uint(xv) * ds_udiv(1u << 24, uint(xdiff))) >> 16;   // the constant-w ramp
  else { uint num = uint(xv * wl) << 8, den = uint(xv * wl) + uint((xdiff - xv) * wr); fac = den == 0u ? 0u : ds_udiv(num, den); }
  if (do_rgb) rgb = ivec3(ds_persp(lrgb.r, rrgb.r, fac), ds_persp(lrgb.g, rrgb.g, fac), ds_persp(lrgb.b, rrgb.b, fac));
  if (do_st) st = ivec2(ds_persp(lst.x, rst.x, fac), ds_persp(lst.y, rst.y, fac));
  return true;
}
#endif
