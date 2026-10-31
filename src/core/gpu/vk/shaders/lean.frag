#version 450
// The lean path's one fragment stage: DS shading (ds_shade.glsl) once per
// pixel, the alpha test, fog by the pixel's own depth, and the colour out as
// raw 6-bit channels with the 5-bit alpha encoded so the pipeline's blend
// factors and MAX alpha reproduce the DS blend. Depth, stencil and blending
// are the pipeline's; nothing here reads the framebuffer.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
layout(std430, binding = 4) readonly buffer Spans  { uvec4 spans[]; };   // the span table (vk_layout.h): DS_SPAN_WORDS = two uvec4 per row
#define DS_TEXEL_BUFFER 1
layout(binding = 2) uniform usamplerBuffer texels_tb;
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat in uint v_poly;
layout(location = 1) in vec3 v_rgb;
layout(location = 2) in vec2 v_st;
layout(location = 3) noperspective in float v_z;
layout(location = 4) in float v_zp;
layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_fog;   // the polygon's fog bit; blended with MIN so a pixel keeps fog only if every layer had it (the DS rule)
#ifdef DS_EARLY
// Polygons whose alpha cannot fail the test (no cut-out texture): no
// discard, so the depth test runs before shading and overdraw costs nothing.
layout(early_fragment_tests) in;
#endif
#include "ds_shade.glsl"

// The DS evaluates a pixel at its top-left corner; with vertices on pixel
// corners that is half a pixel before the centre, so attributes are read
// there. On an edge pixel the corner can lie outside the polygon and the
// value is an extrapolation, so texcoords are clamped to the polygon's own
// range (row_base / pad_ carry it: s min|max, t min|max as 16-bit pairs)
// and colours to their range, never a texel past the polygon.
vec2 st_bounds_lo(GpuPoly p) { return vec2(float(int(p.row_base & 0xFFFFu) - 32768), float(int(p.pad_ & 0xFFFFu) - 32768)); }
vec2 st_bounds_hi(GpuPoly p) { return vec2(float(int(p.row_base >> 16) - 32768), float(int(p.pad_ >> 16) - 32768)); }

// The DS's own attributes for this pixel: the row's span endpoints from the
// span table (vk_layout.h, the CPU raster's Slope/Interp output), then the
// hardware's fixed-point interpolation along the row (kernels_ref.cpp:
// span_factor, span_attr_persp, lin_at), so colour and texcoords equal the
// CPU raster's at every pixel the DS draws. A pixel the GPU's coverage
// reaches past the span reads the nearest endpoint. False: no row (the
// fallback is the hardware's interpolation).
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

void main() {
  GpuPoly p = polys[v_poly];
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  ivec3 rgb9; ivec2 st4;
  if (!ds_attrs(p, textured, rgb9, st4)) {
    vec3 rgb = clamp(interpolateAtOffset(v_rgb, vec2(-0.5)), vec3(0.0), vec3(511.0));
    vec2 st = clamp(interpolateAtOffset(v_st, vec2(-0.5)), st_bounds_lo(p), st_bounds_hi(p));
    // Texcoords truncate as the DS 12.4 >> 4 does; +0.01 absorbs float error.
    rgb9 = ivec3(round(rgb)); st4 = ivec2(floor(st + 0.01));
  }
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  uint src = shade_pixel(p, blendmode, polyalpha, textured, rgb9.r, rgb9.g, rgb9.b, st4.x, st4.y);
  uint alpha = src >> 24;
#ifndef DS_EARLY
  if (alpha <= pc.f.alpha_ref) discard;
#endif
  // A translucent polygon's pixels are opaque where their final alpha is 31
  // (written with depth, no translucent flag, so a later polygon of the
  // same id draws over them) and translucent elsewhere. The run is drawn
  // twice: DS_ALPHA_SEL 1 keeps the opaque pixels, 2 the translucent ones.
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (alpha == 31u) discard;
#endif
  uint r = src & 0x3Fu, g = (src >> 8) & 0x3Fu, b = (src >> 16) & 0x3Fu;
  // Fog is the resolve's (once per pixel by the pixel's depth, as the DS does
  // after blending); the polygon's fog bit goes to the fog attachment.
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  // Alpha byte: 0 stays 0 (undrawn), else (a + 1) * 8: the blend's SRC_ALPHA is (a + 1) / 32 to within 1/256.
  uint ab = alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
  o_fog = float(fogbit);
}
