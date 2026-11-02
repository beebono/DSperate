#version 310 es
#extension GL_OES_shader_multisample_interpolation : enable
// The lean path's fragment stage on GLES (lean.frag is the Vulkan original):
// DS shading once per pixel, the alpha test, the colour out as raw 6-bit
// channels with the 5-bit alpha encoded for the blend, the fog bit and the
// polygon record to their attachments. Variants: DS_EARLY (no alpha test,
// early depth), DS_ALPHA_SEL 1/2 (a translucent run's alpha-31 pixels /
// the rest), DS_WDEPTH (W-buffered frame: depth written here).
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
layout(std430, binding = 4) readonly buffer Spans  { uvec4 spans[]; };
layout(std430, binding = 2) readonly buffer Texels { uint texels[]; };
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
flat in uint v_poly;
in vec3 v_rgb;
in vec2 v_st;
in float v_zp;
layout(location = 0) out vec4 o_col;
layout(location = 1) out vec4 o_fog;
layout(location = 2) out uint o_id;
#ifndef DS_ALPHA_SEL
#define DS_ALPHA_SEL 0
#endif
#if defined(DS_EARLY) && !defined(DS_WDEPTH)
layout(early_fragment_tests) in;
#endif
#include "ds_shade.glsl"
#include "ds_spans.glsl"
#ifdef GL_OES_shader_multisample_interpolation
#define DS_AT_CORNER(v) interpolateAtOffset(v, vec2(-0.5))
#else
#define DS_AT_CORNER(v) ((v) - 0.5 * (dFdx(v) + dFdy(v)))
#endif
vec2 st_bounds_lo(GpuPoly p) { return vec2(float(int(p.row_base & 0xFFFFu) - 32768), float(int(p.pad_ & 0xFFFFu) - 32768)); }
vec2 st_bounds_hi(GpuPoly p) { return vec2(float(int(p.row_base >> 16) - 32768), float(int(p.pad_ >> 16) - 32768)); }
void main() {
#ifdef DS_WDEPTH
  gl_FragDepth = v_zp / 16777215.0;
#endif
  GpuPoly p = polys[v_poly];
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  ivec3 rgb9; ivec2 st4;
  if (!ds_attrs(p, textured, rgb9, st4)) {
    vec3 rgb = clamp(DS_AT_CORNER(v_rgb), vec3(0.0), vec3(511.0));
    vec2 st = clamp(DS_AT_CORNER(v_st), st_bounds_lo(p), st_bounds_hi(p));
    rgb9 = ivec3(round(rgb)); st4 = ivec2(floor(st + 0.01));
  }
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  uint src = shade_pixel(p, blendmode, polyalpha, textured, rgb9.r, rgb9.g, rgb9.b, st4.x, st4.y);
  uint alpha = src >> 24;
#ifndef DS_EARLY
  if (alpha <= pc.f.alpha_ref) discard;
#endif
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (alpha == 31u) discard;
#endif
  uint r = src & 0x3Fu, g = (src >> 8) & 0x3Fu, b = (src >> 16) & 0x3Fu;
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  uint ab = alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  o_col = vec4(float(r), float(g), float(b), float(ab)) / 255.0;
  o_fog = vec4(float(fogbit), 0.0, 0.0, 1.0);
  o_id = v_poly + 1u;
}
