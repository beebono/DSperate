#version 450
// The lean path's one fragment stage: DS shading (ds_shade.glsl) once per
// pixel, the alpha test, fog by the pixel's own depth, and the colour out as
// raw 6-bit channels with the 5-bit alpha encoded so the pipeline's blend
// factors and MAX alpha reproduce the DS blend. Depth, stencil and blending
// are the pipeline's; nothing here reads the framebuffer.
#extension GL_GOOGLE_include_directive : require
#if defined(DS_FETCH) && DS_FETCH == 1
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require   // shaderFloat16 (vk_lean.cpp requires it for this build)
#endif
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };   // read by the debug build alone (DS_POLY_DEBUG)
#define DS_TEXEL_BUFFER 1
layout(binding = 2) uniform usamplerBuffer texels_tb;
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat in uint v_poly;
layout(location = 1) in vec3 v_rgb;   // the vertices' colour and texture coordinates, perspective-correct (lean.vert)
layout(location = 2) in vec2 v_st;
layout(location = 3) noperspective in float v_z;
layout(location = 4) in float v_zp;
// The polygon's words, made ready by the vertex stage (lean.vert poly_out): on the G52 every operation here is paid
// per pixel, and unpacking the record's fields was 6% of a 4x frame.
layout(location = 6) flat in ivec4 v_pb;   // the texcoord range: s and t min, s and t max
layout(location = 8) flat in ivec4 v_tex;  // arena offset, width, blend mode | alpha << 2 | fog bit << 7 | textured << 8, the edge-marking word's low bits
layout(location = 9) noperspective in vec3 v_edge;   // under 1 within a DS pixel of a side that is the polygon's outline (lean.vert)
layout(location = 7) flat in ivec4 v_wrap; // the texture wrap: the mask of s and t, the mirror's fold (lean.vert)
#define DS_WRAP_VARY v_wrap
layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_fog;   // the polygon's fog bit; blended with MIN so a pixel keeps fog only if every layer had it (the DS rule)
// Edge marking's word (lean_mark.frag reads it), kept by the opaque passes
// only. Without the attachment the write goes nowhere.
layout(location = 2) out uint o_id;
#ifdef DS_EARLY
// Polygons whose alpha cannot fail the test (no cut-out texture): no
// discard, so the depth test runs before shading and overdraw costs nothing.
layout(early_fragment_tests) in;
#endif
#ifdef DS_FETCH
// The tile's colour (subpass 0 reads its own attachment in rasterisation order, VK_EXT_rasterization_order_attachment_access).
layout(input_attachment_index = 0, set = 1, binding = 3) uniform subpassInput fb_col;
#endif
layout(constant_id = 2) const bool ds_toon = true;   // false: no mode-2 polygon this frame (vk_lean.cpp picks the pipeline set)
#define DS_TOON ds_toon
// The colour's alpha byte is the record's, the 5-bit alpha itself, not the blend unit's (a + 1) * 8 (vk_lean.cpp
// Impl::raw: nothing blends by the byte, and the tile is the record as drawn), with the pixel's fog flag in bit 7
// (no fog attachment: a translucent pixel keeps the flag only if the pixel beneath had it, here in the shader).
layout(constant_id = 4) const bool ds_raw = false;
layout(constant_id = 5) const bool ds_keep31 = false;   // DS_ALPHA_SEL 2 drawing a shadow: the pixels of alpha 31 too
// false: the pipelines of a frame that marks no edges (vk_lean.cpp pipe_nm, no id attachment). Untaken, the id word's
// code still cost Golden Sun: Dark Dawn's 4x frame 0.9 ms of 23.
layout(constant_id = 6) const bool ds_mark = true;
// A multisampled frame resolved by the hardware (vk_lean.cpp Impl::mshw): a drawn sample's colour bytes are
// its channels + 192, so the resolved mean tells how many of a pixel's samples were drawn (lean_msdec.frag).
// The blend unit's sum of two such bytes, weights adding to one, is one again.
layout(constant_id = 7) const bool ds_msr = false;
#include "ds_shade.glsl"


void main() {
  uint vpoly = v_poly & 0xFFFu;
  GpuPoly p;   // from the vertex stage: no load of the record here
  p.first_vert = 0u; p.nverts = 0u; p.ytop = 0; p.ybot = 0; p.span_base = 0u; p.vbot = 0u; p.xmin = 0; p.xmax = 0;
  p.attr = 0u; p.texparam = 0u; p.flags = 0u; p.tex_h = 0u; p.row_base = 0u; p.pad_ = 0u;
  p.tex_offset = uint(v_tex.x); p.tex_w = uint(v_tex.y);
  bool textured = (v_tex.z & 256) != 0;
  // Texture coordinates to 12.4, floored as the DS does (the 1/32 keeps float error off the texel before on a sprite
  // drawn texel for pixel; to the nearest, a texel column stretched over a quad read the next column, a transparent
  // one, on its last pixels: Etrian Odyssey's buttons and text boxes), then clamped to the polygon's own range, so
  // an edge pixel never reads a texel past the polygon.
  ivec2 st4 = clamp(ivec2(floor(v_st + 0.03125)), v_pb.xy, v_pb.zw);
  // The colour is made in float: the alpha stays the DS's integer (every test on it is exact) and the
  // colour comes out within one of the DS's 63 levels (no truncation after each product). On the G52 the integer
  // form was 0.2-0.7 of the stage's 2.9-4.3 arithmetic cycles; with the varyings above, 5-12% of a 4x frame.
  DS_MP uint blendmode = uint(v_tex.z) & 3u, polyalpha = (uint(v_tex.z) >> 2) & 31u;
  uint fogbit = (uint(v_tex.z) >> 7) & 1u;
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
  // A translucent polygon's pixels are opaque where their alpha is 31 and translucent elsewhere: DS_ALPHA_SEL 1
  // keeps the opaque pixels, 2 the translucent ones.
#if DS_ALPHA_SEL == 1
  if (alpha != 31u) discard;
#elif DS_ALPHA_SEL == 2
  if (!ds_keep31 && alpha == 31u) discard;
#endif
#endif
  vec3 vc = floor(clamp(v_rgb, vec3(0.0), vec3(511.0)) * 0.125);   // the vertex colour's 6 bits
  bool mode2 = ds_toon && blendmode == 2u, highlight = (pc.f.dispcnt & 2u) != 0u;
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
  DS_MP uint ab = ds_raw ? (alpha | (fogbit << 7)) : alpha == 0u ? 0u : min(255u, (alpha + 1u) * 8u);
  // Cut off, as the DS's shifts do: to the nearest (the attachment's own rounding), a texture under a black vertex
  // colour came out 0 or 1 by its texels where the DS has 0 throughout, and SM64's title showed its backdrop's
  // pattern through the fade laid over it.
  // (A floor, not the value sent out just under a half lower for the attachment to round: a conversion may land
  // either side of a half, and on the host it did.)
  col = floor(col + 1e-3);
#ifdef DS_POLY_DEBUG
  // DS_R3D_POLYDUMP (render3d.cpp poly_dump; tools/polydump.sh builds these stages): the polygon's number for the colour.
#if DS_POLY_DEBUG == 2
  col = vec3(float((st4.x >> 4) & 63), float((st4.y >> 4) & 63), float(((st4.x >> 2) & 3) | (((st4.y >> 2) & 3) << 2)));
#else
  { uint dn = polys[vpoly].vbot >> 16; col = vec3(float(dn & 63u), float((dn >> 6) & 63u), 0.0); }
#endif
#endif
  vec3 col_out = ds_msr ? min(col, vec3(63.0)) + 192.0 : col;
  o_col = vec4(col_out, float(ab)) * (1.0 / 255.0);
#ifdef DS_FETCH
  // A translucent run in one pass: the pixel beneath read from the tile. Alpha 0 beneath: written as is; else
  // blended, alpha the larger. The fog flag is kept only if the pixel beneath had it.
  vec4 dst = subpassLoad(fb_col);
  uint dab = uint(dst.a * 255.0 + 0.5), dfog = 0u;
  if (ds_raw) { dfog = dab & 0x80u & (fogbit << 7); dab &= 0x1Fu; o_col.a = float(alpha | dfog) * (1.0 / 255.0); }
  if (dab != 0u) {
    vec3 c = col_out;
    if ((pc.f.dispcnt & 8u) != 0u) c = floor(mix(dst.rgb * 255.0, col, float(alpha + 1u) * (1.0 / 32.0)) + 1e-3);   // cut off again: the DS's shift (the 1e-3 covers the byte's float error)
    o_col = vec4(c, float(ds_raw ? (max(uint(alpha), dab) | dfog) : max(uint(ab), dab))) * (1.0 / 255.0);
  }
#endif
  o_fog = float(fogbit);
  // Edge marking (lean_mark.frag): the polygon's id, fog bit, "drawn", whether the pixel is one of the
  // polygon's edge and its DS depth less a bit, where a frame marks edges.
  if (ds_mark && (pc.f.flags2 & DS_FF2_EDGEMARK) != 0u) {
    highp float dz = (pc.f.flags & DS_FF_WBUFFER) != 0u ? v_zp : v_z;
    // A pixel of the polygon's edge: within a DS pixel of its outline, along x or along y (the DS's edge runs).
    // Every pixel was one before, and a texture that cuts texels out was outlined texel by texel (Solatorobo's clouds).
    o_id = uint(v_tex.w) | (any(lessThan(v_edge, vec3(1.0))) ? 0x100u : 0u) | ((uint(clamp(dz, 0.0, 16777215.0)) >> 1) << 9);
  } else o_id = vpoly + 1u;
}
