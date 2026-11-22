#version 310 es
#extension GL_OES_shader_multisample_interpolation : enable
// The lean path's fragment stage on GLES (lean.frag is the Vulkan original):
// DS shading once per pixel, the alpha test, the colour out as raw 6-bit
// channels with the 5-bit alpha encoded for the blend, the fog bit and the
// polygon record to their attachments. Variants: DS_EARLY (no alpha test,
// early depth), DS_ALPHA_SEL 1/2 (a translucent run's alpha-31 pixels /
// the rest), DS_WDEPTH (W-buffered frame: depth written here), DS_WIRE (a
// run of wireframe polygons: the edge test; no other build has it, nor
// the polygon records and the vertex texture it reads).
#ifdef DS_WIRE
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
#endif
layout(std430, binding = 2) readonly buffer Texels { uint texels[]; };
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
flat in uint v_poly;
in vec3 v_rgb;
in vec2 v_st;
in float v_zp;
// The polygon's words, made ready by the vertex stage (gl_lean.vert poly_out): no load of the record here.
flat in ivec4 v_pb;
flat in ivec4 v_wrap;
flat in ivec4 v_tex;
in vec3 v_edge;       // gl_lean.vert: the distance from each outline side, times w
flat in int v_edir;
#define DS_WRAP_VARY v_wrap
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
#ifdef DS_WIRE
// A wireframe polygon (alpha 0): the DS draws its edges only, a pixel a row
// along a steep one and a pixel a column along a shallow one. The fan covers
// the polygon; kept is what lies within a pixel of an edge, inside it. The
// vertices come from the vertex stage's texture (three texels each, the
// 28.4 position in the third). Such polygons are never drawn with the early
// depth test, and are drawn in runs of their own by this build (gl_lean.cpp).
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
  // As lean.frag, the Vulkan stage: the alpha first (the DS's integer, every test on it exact), then the colour in
  // float, within one of the DS's 63 levels. The integer form of ds_shade.glsl with the record loaded and unpacked
  // here was 25.6 arithmetic cycles on the G31 by the offline compiler, a third of them the unpacking and the wrap.
  GpuPoly p;
  p.tex_offset = uint(v_tex.x); p.tex_w = uint(v_tex.y);
  bool textured = (v_tex.z & 256) != 0;
  ivec2 st4 = clamp(ivec2(floor(v_st + 0.03125)), v_pb.xy, v_pb.zw);
  DS_MP uint blendmode = uint(v_tex.z) & 3u, polyalpha = (uint(v_tex.z) >> 2) & 31u;
  uint fogbit = (uint(v_tex.z) >> 7) & 1u;
#ifdef DS_WIRE
  {   // opaque lines (the record is read for these alone)
    GpuPoly wp = polys[v_poly];
    if (!wire_keeps(wp)) discard;
  }
#endif
  DS_MP uint ta = 31u;
  vec3 tc = vec3(63.0);
  if (textured) {
    uint texw = sample_tex(p, st4.x, st4.y);
    ta = (texw >> 16) & 0x1Fu;
    vec3 t5 = vec3(uvec3(texw, texw >> 5, texw >> 10) & 31u);
    tc = t5 * 2.0 + step(0.5, t5);   // 5 bits to 6: doubled, + 1 unless 0
  }
  DS_MP uint alpha = (!textured || (blendmode & 1u) != 0u) ? polyalpha : ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
#ifndef DS_EARLY
  if (alpha <= pc.f.alpha_ref) discard;
#endif
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (alpha == 31u) discard;
#endif
  vec3 vc = floor(clamp(v_rgb, vec3(0.0), vec3(511.0)) * 0.125);   // the vertex colour's 6 bits
  bool mode2 = DS_TOON && blendmode == 2u, highlight = (pc.f.dispcnt & 2u) != 0u;
  vec3 toon = vec3(0.0);
  if (mode2) {
    uint tw = ps.toon[uint(round(clamp(v_rgb.r, 0.0, 511.0))) >> 4];
    vec3 t5 = vec3(uvec3(tw, tw >> 5, tw >> 10) & 31u);
    toon = t5 * 2.0 + step(0.5, t5);
    vc = highlight ? vc.rrr : toon;
  }
  vec3 col = vc;
  if (textured) {
    if ((blendmode & 1u) != 0u) col = mix(vc, tc, ta == 31u ? 1.0 : float(ta) * (1.0 / 32.0));   // decal
    else col = ((tc + 1.0) * (vc + 1.0) - 1.0) * (1.0 / 64.0);                                      // modulate
  }
  if (mode2 && highlight) col = min(col + toon, vec3(63.0));
  col = floor(col + 1e-3);   // cut off, as the DS's shifts do (lean.frag)
  DS_MP uint ab = alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  highp uvec4 src = uvec4(uvec3(col), alpha);
  highp vec4 c = vec4(col, float(ab));   // highp before the divide: the blend unit takes the value as is, and fp16's error moved blends by 1
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
#ifdef DS_EDGE_AA
  // Edge anti-aliasing (DS_EDGE_AA: the programs of a frame with the game's bit under video.aa): a pixel whose
  // centre is under half a pixel inside the polygon's outline is covered by the polygon for half a pixel and that
  // distance more; the rest is the neighbour's across the side. The byte: the neighbour's direction << 6, the
  // distance over half a pixel in 0-62; 255: no edge. With it the polygon's id (what lies across an edge of the
  // same object is not blended in). Kept from the opaque passes alone (gl_lean.cpp's write mask);
  // gl_resolve.frag does the blend.
  float ecode = 1.0;
#if DS_ALPHA_SEL != 2
  {
    vec3 e = v_edge * gl_FragCoord.w;
    float ed = min(e.x, min(e.y, e.z));
    if (ed < 0.5) {
      int side = (e.x <= e.y && e.x <= e.z) ? 0 : (e.y <= e.z ? 1 : 2);
      ecode = float(((v_edir >> (2 * side)) & 3) * 64 + int(clamp(ed, 0.0, 0.4999) * 126.0)) / 255.0;
    }
  }
#endif
  o_fog = vec4(float(fogbit), ecode, float(v_tex.w & 63) / 255.0, 1.0);
#else
  o_fog = vec4(float(fogbit), 0.0, 0.0, 1.0);
#endif
  // Edge marking (gl_mark.frag): the polygon's id, fog bit, "drawn" and the pixel's DS depth, where a frame marks edges.
  // A polygon whose texture cuts texels out (bit 8) is "drawn" for the marking within a pixel of its own outline
  // alone: a hole's rim is not an edge of the polygon, and was outlined texel by texel (Solatorobo's clouds).
  if ((pc.f.flags2 & DS_FF2_EDGEMARK) != 0u) {
    uint idw = uint(v_tex.w) & 0xFFu;
    if ((v_tex.w & 256) != 0) { vec3 em = v_edge * gl_FragCoord.w; if (min(em.x, min(em.y, em.z)) >= 1.0) idw &= 0x7Fu; }
    o_id = idw | (uint(clamp(v_zp, 0.0, 16777215.0)) << 8);
  }
  else o_id = v_poly + 1u;
}
