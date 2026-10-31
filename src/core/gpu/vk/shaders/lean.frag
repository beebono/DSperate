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
layout(std430, binding = 4) readonly buffer Spans  { uint spans[]; };   // the span table (vk_layout.h)
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

// The DS fills a row from the left edge's whole pixel to the right edge's,
// and runs the attributes from the left edge's value to the right edge's
// over exactly those pixels: pixel x reads the polygon's plane not at x but
// at xl + (x - floor(xl)) * (xr - xl) / (ceil(xr) - floor(xl)), the edges
// at this row taken over the whole-pixel vertices the hardware has. A
// vertical edge gives no shift (sprites stay texel-exact); a slanted one
// shifts the reading up to a pixel, differently each row, and without it
// textured ground flipped texels against the CPU raster all over. The
// row's xl and step come from the span table, built at submit.
// Approximate for the hardware's X-major edge runs; those rows are few.
float span_shift(GpuPoly p) {
  if (p.span_base == DS_SPAN_NONE) return 0.0;
  int y = int(gl_FragCoord.y), ry0 = max(p.ytop, 0), ry1 = min(p.ybot, 191);
  if (y < ry0 || y > ry1) return 0.0;   // a fragment past the whole-pixel extent
  uint i = uint(DS_SPAN_WORDS) * (p.span_base + uint(y - ry0));
  float xl = uintBitsToFloat(spans[i]), step = uintBitsToFloat(spans[i + 1u]);
  float x = floor(gl_FragCoord.x);
  return xl + (x - floor(xl)) * step - x;
}

void main() {
  GpuPoly p = polys[v_poly];
  float xs = span_shift(p);   // where along the row the DS reads this pixel's attributes
  vec3 rgb = clamp(interpolateAtOffset(v_rgb, vec2(-0.5)) + dFdxFine(v_rgb) * xs, vec3(0.0), vec3(511.0));
  vec2 st = clamp(interpolateAtOffset(v_st, vec2(-0.5)) + dFdxFine(v_st) * xs, st_bounds_lo(p), st_bounds_hi(p));
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  // Texcoords truncate as the DS 12.4 >> 4 does; +0.01 absorbs float error.
  uint src = shade_pixel(p, blendmode, polyalpha, textured, int(round(rgb.r)), int(round(rgb.g)), int(round(rgb.b)), int(floor(st.x + 0.01)), int(floor(st.y + 0.01)));
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
