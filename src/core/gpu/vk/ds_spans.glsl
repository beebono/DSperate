// The DS's own per-pixel attributes from the span table, for the draw
// (lean.frag) and the edge cut's cut-out pass (lean_cutout.frag), which
// must sample the texel the draw did. Needs `spans` (binding 4) and
// GpuPoly in scope.
#ifndef DS_SPANS_GLSL
#define DS_SPANS_GLSL
// DS_FRAG_SCALE: device pixels per DS pixel (the lean draw passes the frame's scale; 1 elsewhere).
#ifndef DS_FRAG_SCALE
#define DS_FRAG_SCALE 1
#endif
// Exact (the DS's integer interpolation, equal to the CPU raster) or float
// (one factor per pixel, no integer divides) along the row. Float is the
// default: ~2.6 ms less GPU on Pokémon White at 1x, 7 under 4x MSAA, where
// the exact path's divides cost 2-3x more per fragment; it differs from
// exact on 1-5% of drawn pixels, a texel step one pixel early or late.
// DS_SPAN_EXACT=1 picks exact (vk_lean.cpp: a specialization constant, so
// each pipeline holds one path; gl_lean.cpp: a #define).
#if defined(VULKAN)
layout(constant_id = 0) const bool ds_span_exact = false;
#elif defined(DS_SPAN_EXACT)
const bool ds_span_exact = true;
#else
const bool ds_span_exact = false;
#endif
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
// hardware's interpolation along the row (kernels_ref.cpp: span_factor,
// span_attr_persp, lin_at): exact, so colour and texcoords equal the CPU
// raster's at every pixel the DS draws, or its float form (ds_span_exact
// above). A pixel the GPU's coverage
// reaches past the span reads the nearest endpoint. Equal endpoints (flat
// colour, most rows) and an untextured polygon's texcoords skip the
// arithmetic, which under MSAA runs for every edge fragment of this scene's
// many small polygons. False: no row (the fallback is the hardware's
// interpolation).
bool ds_attrs(GpuPoly p, bool textured, out ivec3 rgb, out ivec2 st) {
  if (p.span_base == DS_SPAN_NONE) return false;
  ivec2 fc = ivec2(gl_FragCoord.xy) / DS_FRAG_SCALE;   // the DS pixel this fragment lies in
  int y = fc.y, ry0 = max(p.ytop, 0), ry1 = min(p.ybot, 191);
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
  int xv = clamp(fc.x - xstart, 0, xdiff - 1);
  ivec3 rrgb = ivec3(b.z & 0x3FFu, (b.z >> 10) & 0x3FFu, (b.z >> 20) & 0x3FFu);
  ivec2 rst = ivec2(int(b.w << 16) >> 16, int(b.w) >> 16);
  if (!ds_span_exact) {
    // The same shape in float: one factor for every channel, each value
    // floored up from its lower endpoint as the DS's does.
    float t;
    // (The Mali compiler is sensitive to this branch's shape: linear and the
    // constant-w ramp as two branches measured 0.7-1.4 ms faster under 4x
    // MSAA than one, and 0.15-0.2 slower at 1x, which has the headroom.)
    if (wl == wr && (wl & 0x7F) == 0) t = float(xv) / float(xdiff);   // linear
    else if (wl == wr) t = float(xv) / float(xdiff);   // the constant-w ramp
    else { float a = float(xv) * float(wl), c = float(xdiff - xv) * float(wr); t = a + c == 0.0 ? 0.0 : a / (a + c); }
    if (do_rgb) {
      vec3 l = vec3(lrgb), r = vec3(rrgb);
      rgb = ivec3(min(l, r) + floor(abs(r - l) * mix(vec3(1.0 - t), vec3(t), lessThan(l, r)) + 1e-3));
    }
    if (do_st) {
      vec2 l = vec2(lst), r = vec2(rst);
      st = ivec2(min(l, r) + floor(abs(r - l) * mix(vec2(1.0 - t), vec2(t), lessThan(l, r)) + 1e-3));
    }
    return true;
  }
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
