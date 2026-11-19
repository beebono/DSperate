#version 310 es
#extension GL_OES_shader_multisample_interpolation : enable
// The lean path's fragment stage on GLES (lean.frag is the Vulkan original):
// DS shading once per pixel, the alpha test, the colour out as raw 6-bit
// channels with the 5-bit alpha encoded for the blend, the fog bit and the
// polygon record to their attachments. Variants: DS_EARLY (no alpha test,
// early depth), DS_ALPHA_SEL 1/2 (a translucent run's alpha-31 pixels /
// the rest), DS_WDEPTH (W-buffered frame: depth written here).
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
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
// The polygon's texcoord range (row_base / pad_: s and t min | max << 16, each + 32768).
ivec2 st_bounds_lo(GpuPoly p) { return ivec2(int(p.row_base & 0xFFFFu), int(p.pad_ & 0xFFFFu)) - 32768; }
ivec2 st_bounds_hi(GpuPoly p) { return ivec2(int(p.row_base >> 16), int(p.pad_ >> 16)) - 32768; }
#ifndef DS_EARLY
// A wireframe polygon (alpha 0): the DS draws its edges only, a pixel a row
// along a steep one and a pixel a column along a shallow one. The fan covers
// the polygon; kept is what lies within a pixel of an edge, inside it. The
// vertices come from the vertex stage's texture (three texels each, the
// 28.4 position in the third). Such polygons are never drawn with the early
// depth test (gl_lean.cpp).
uniform highp usampler2D verttex;
highp vec2 wire_vert(uint i, highp float sc) {
  uint t = i * 3u + 2u;
  uvec4 c = texelFetch(verttex, ivec2(int(t & 1023u), int(t >> 10u)), 0);
  return vec2(float(int(c.y)), float(int(c.z))) * sc;
}
bool wire_keeps(GpuPoly p) {
  highp vec2 c = gl_FragCoord.xy;
  highp float sc = float(max(pc.f.scale, 1u)) / 16.0;
  uint n = min(p.nverts, 10u);
  highp vec2 first = wire_vert(p.first_vert, sc), a = first;
  for (uint i = 1u; i <= n; ++i) {
    highp vec2 b = i < n ? wire_vert(p.first_vert + i, sc) : first;
    highp vec2 d = b - a;
    if (abs(d.y) >= abs(d.x)) {
      if (d.y != 0.0 && c.y >= min(a.y, b.y) - 0.5 && c.y <= max(a.y, b.y) + 0.5 && abs(c.x - (a.x + d.x * (c.y - a.y) / d.y)) < 1.0) return true;
    } else if (c.x >= min(a.x, b.x) - 0.5 && c.x <= max(a.x, b.x) + 0.5 && abs(c.y - (a.y + d.y * (c.x - a.x) / d.x)) < 1.0) return true;
    a = b;
  }
  return false;
}
#endif
void main() {
#ifdef DS_WDEPTH
  gl_FragDepth = v_zp / 16777215.0;
#endif
  GpuPoly p = polys[v_poly];
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  // The vertices' colour, and their texture coordinates floored as the DS does and clamped to the polygon's range (lean.frag).
  ivec3 rgb9 = ivec3(round(clamp(v_rgb, vec3(0.0), vec3(511.0))));
  ivec2 st4 = clamp(ivec2(floor(v_st + 0.03125)), st_bounds_lo(p), st_bounds_hi(p));
  DS_MP uint blendmode = (p.attr >> 4) & 3u;
  DS_MP uint polyalpha = (p.attr >> 16) & 0x1Fu;
  if (polyalpha == 0u) {   // wireframe: opaque lines
#ifndef DS_EARLY
    if ((p.flags & (DS_PF_SHADOW | DS_PF_SHADOW_MASK)) == 0u && !wire_keeps(p)) discard;
#endif
    polyalpha = 31u;
  }
  DS_MP uvec4 src = shade_pixel(p, blendmode, polyalpha, textured, rgb9.r, rgb9.g, rgb9.b, st4.x, st4.y);
  DS_MP uint alpha = src.a;
#ifndef DS_EARLY
  if (alpha <= pc.f.alpha_ref) discard;
#endif
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (alpha == 31u) discard;
#endif
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  DS_MP uint ab = alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  highp vec4 c = vec4(uvec4(src.rgb, ab));   // highp before the divide: the blend unit takes the value as is, and fp16's error moved blends by 1
  o_col = c / 255.0;
#ifdef DS_FETCH
  // A translucent run in one pass (B3): the pixel beneath read from the
  // tile. Alpha 0 beneath (the rear plane without alpha, nothing drawn):
  // written as is; else blended, alpha the larger (melonDS AlphaBlend).
  // The stencil's same-id rule and the fog bit's MIN stay the pipeline's.
  highp vec4 dst = gl_LastFragColorARM;
  highp uint dab = uint(round(dst.a * 255.0));
  if (dab != 0u) {
#if DS_FETCH == 2
    // The DS's own blend on the 6-bit channels (DISP3DCNT bit 3 off: no colour blend).
    highp uvec3 s6 = uvec3(src.rgb);
    if ((pc.f.dispcnt & 8u) != 0u) s6 = (s6 * (alpha + 1u) + uvec3(round(dst.rgb * 255.0)) * (31u - alpha)) >> 5;
    o_col = vec4(uvec4(s6, max(uint(ab), dab))) / 255.0;
#else
    // As the blend unit does it, which on Mali is at fp16 (fp32 maths moved
    // ~0.5% of blended pixels by 1): SRC_ALPHA / ONE_MINUS_SRC_ALPHA, alpha MAX.
    mediump vec4 sm = o_col, dm = dst;
    o_col = vec4(sm.rgb * sm.a + dm.rgb * (1.0 - sm.a), max(sm.a, dm.a));
#endif
  }
#endif
  o_fog = vec4(float(fogbit), 0.0, 0.0, 1.0);
  // Edge marking (gl_mark.frag): the polygon's id, fog bit, "drawn" and the pixel's DS depth, where a frame marks edges.
  if ((pc.f.flags2 & DS_FF2_EDGEMARK) != 0u) o_id = ((p.attr >> 24) & 0x3Fu) | (fogbit << 6) | 0x80u | (uint(clamp(v_zp, 0.0, 16777215.0)) << 8);
  else o_id = v_poly + 1u;
}
