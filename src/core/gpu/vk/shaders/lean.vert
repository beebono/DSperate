#version 450
// The lean path: polygons as fans through the hardware rasteriser, one
// instance per polygon. Each is the game's own triangles: the vertices on their
// 28.4 sub-pixel positions (the DS's own, before it drops the fraction) with
// their own attributes, nothing moved, grown or redrawn. That is what
// DeSmuME's OpenGL renderer does, the picture taken as right for a hardware
// raster (dsperate-research/tools/desmume, ref3d --gl: NSMB's attract mode at
// 1x differs from it on 1-2 pixels a frame), with and without MSAA.
// What was tried to bring one sample at the pixel's centre nearer the DS's
// fill, and moved the picture away instead: a quarter-pixel pull towards the
// pixel the DS lights, 1/16 px of growth from the polygon's centre, the
// attributes read from whole-pixel planes, a polygon inside one pixel row or
// column redrawn as a line, vertices on whole pixels, and the DS's fill itself
// from a table of rows (exact, and at 1x the dearer).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;   // the polygon
// The vertex's 9-bit colour and 12.4 texture coordinates, perspective-correct.
layout(location = 1) out vec3 v_rgb;
layout(location = 2) out vec2 v_st;
layout(location = 3) noperspective out float v_z;    // DS z, linear across the screen (Z-buffer mode)
layout(location = 4) out float v_zp;                 // DS depth, perspective-correct (W-buffer mode)
// The polygon's words the fragment stage shades by, so it loads nothing of the polygon itself (the G52's fragment stage is bound by its load/store unit, and the record's fields were six of its ten cycles).
layout(location = 6) flat out ivec4 v_pb;   // the texcoord range: s and t min, s and t max (a clamped axis: within the texture)
layout(location = 7) flat out ivec4 v_wrap; // texel = min(c & mask, fold - (c & mask)): mask.st, fold.st
// One varying fewer is 0.4 of the fragment stage's 3.6 varying cycles on the G52, which bound the opaque stages: the
// polygon's small fields share v_tex's two free words.
layout(location = 8) flat out ivec4 v_tex;  // arena offset, width, blend mode | alpha << 2 (a wireframe's 0 as 31) | fog bit << 7 | textured << 8, the edge-marking word's low bits
// Edge marking on a fan: how far the pixel is from the polygon's own outline, as the triangle's barycentric
// coordinate against each side that is one (0 on the side, 1 at the vertex across; a side inside the polygon
// holds 1 throughout). The DS marks a polygon's edge pixels alone, not where its texture cuts texels out.
layout(location = 9) noperspective out vec3 v_edge;
void poly_out(GpuPoly p) {
  uint pa = (p.attr >> 16) & 0x1Fu;
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  ivec2 wh = ivec2(int(p.tex_w), int(p.tex_h));
  bvec2 rep = notEqual(ivec2(p.texparam) & ivec2(0x10000, 0x20000), ivec2(0));
  bvec2 flp = bvec2(rep.x && (p.texparam & 0x40000u) != 0u, rep.y && (p.texparam & 0x80000u) != 0u);
  ivec2 lo = ivec2(int(p.row_base & 0xFFFFu), int(p.pad_ & 0xFFFFu)) - 32768, hi = ivec2(int(p.row_base >> 16), int(p.pad_ >> 16)) - 32768;
  // The DS reads no pixel at a span's far end: a coordinate never reaches the polygon's largest. Reached here by a
  // sixteenth's rounding at the last raster pixel, on a repeating texture it wrapped to the texture's other side
  // (a sprite ending on s = width: a column of its atlas's first texels down its edge, Solatorobo's cockpit).
  hi = max(hi - 1, lo);
  v_pb = ivec4(mix(clamp(lo, ivec2(0), wh * 16 - 1), lo, rep), mix(clamp(hi, ivec2(0), wh * 16 - 1), hi, rep));
  v_wrap = ivec4(mix(ivec2(0xFFFF), mix(wh - 1, 2 * wh - 1, flp), rep), mix(ivec2(0xFFFFF), 2 * wh - 1, flp));
  v_tex = ivec4(int(p.tex_offset), wh.x, int(((p.attr >> 4) & 3u) | ((pa == 0u ? 31u : pa) << 2) | (fogbit << 7) | ((p.flags & DS_PF_TEXTURED) != 0u ? 256u : 0u)),
               int(((p.attr >> 24) & 0x3Fu) | (fogbit << 6) | 0x80u));
}
void main() {
  uint pi = uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  // A polygon of four or more vertices is a fan around its centre (melonDS's "improved polygon
  // splitting", GPU3D_OpenGL.cpp, adapted): the centre takes the vertices' mean position and the mean
  // of their attributes over w. The DS walks a polygon's edges and then the row between them, and a
  // quad whose texcoords are not on one plane (a rectangle of texture on a quad that is no
  // parallelogram) is a smooth warp there; from its first vertex it is two planes and a crease, the
  // texture up to a dozen texels off. Around the centre the error is spread over four triangles:
  // Phantom Hourglass's sea and clouds. A polygon inside one column or row of the raster's pixels
  // keeps the fan from its first vertex.
  // A wireframe polygon (alpha 0) is left out (the DS draws its outline only).
  if (((p.attr >> 16) & 0x1Fu) == 0u && (p.flags & (DS_PF_SHADOW | DS_PF_SHADOW_MASK)) == 0u) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_zp = 0.0; return; }
  bool cfan = (pc.f.flags2 & DS_FF2_CENTRE) != 0u && p.nverts >= 4u;
  if (cfan) {
    float gs0 = float(max(pc.f.scale, 1u));
    vec2 lo = vec2(1e9), hi = vec2(-1e9);
    for (uint j = 0u; j < p.nverts; ++j) { vec2 gp = floor(vec2(float(verts[p.first_vert + j].fx), float(verts[p.first_vert + j].fy)) * gs0 / 16.0); lo = min(lo, gp); hi = max(hi, gp); }
    if (lo.x == hi.x || lo.y == hi.y) cfan = false;
  }
  if (cfan ? tri >= p.nverts : tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_zp = 0.0; return; }
  // The fan triangle's three vertices; this one is vt.
  GpuVert ta = verts[p.first_vert], tb = verts[p.first_vert + tri + 1u], tc = verts[p.first_vert + tri + 2u];
  if (cfan) {
    tb = verts[p.first_vert + tri]; tc = verts[p.first_vert + (tri + 1u == p.nverts ? 0u : tri + 1u)];
    float n = float(p.nverts), iw = 0.0, zs = 0.0;
    vec2 cp = vec2(0.0), cst = vec2(0.0); vec3 crgb = vec3(0.0);
    bool wb = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    for (uint j = 0u; j < p.nverts; ++j) {
      GpuVert q = verts[p.first_vert + j];
      float qw = 1.0 / max(float(q.w), 1.0);
      cp += vec2(float(q.fx), float(q.fy)); iw += qw;
      cst += vec2(float(q.s), float(q.t)) * qw; crgb += vec3(float(q.r), float(q.g), float(q.b)) * qw;
      zs += wb ? float(q.z) * qw : float(q.z);
    }
    float cw = 1.0 / iw;   // n / sum(1 / w) is the centre's w; the attributes' sums over w times cw are their means
    ta.fx = int(cp.x / n); ta.fy = int(cp.y / n); ta.sx = ta.fx >> 4; ta.sy = ta.fy >> 4;
    ta.w = int(n * cw + 0.5);
    ta.s = int(floor(cst.x * cw + 0.5)); ta.t = int(floor(cst.y * cw + 0.5));
    ta.r = int(crgb.r * cw + 0.5); ta.g = int(crgb.g * cw + 0.5); ta.b = int(crgb.b * cw + 0.5);
    // To the nearest: cut off, a flat polygon's centre came out one nearer than its vertices on the G52 (5028.9997
    // for 5029), and a shadow drawn at its sprite's own depth, which the DS's test refuses, showed through the
    // sprite between the fan's seams (SM64's file select, the arrow).
    ta.z = int(floor((wb ? zs * cw : zs / n) + 0.5));
  }
  GpuVert vt = ta; if (k == 1u) vt = tb; else if (k == 2u) vt = tc;
  float w = max(float(vt.w), 1.0);
  // The vertex on its 28.4 position: an edge at x covers [x, x1) exactly, whatever the sample pattern.
  float sxf = float(vt.fx) / 16.0, syf = float(vt.fy) / 16.0;
  float x = sxf / 256.0 * 2.0 - 1.0, y = syf / 192.0 * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z/2^24 linear in screen space. W-buffer: vt.z is already the
  // per-polygon normalised depth the DS compares; 1/z interpolated
  // perspective-correct reproduces it, and the test runs GREATER.
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  // Depth-equal polygons (POLYGON_ATTR bit 14: decals on a surface) pass
  // within a tolerance of the pixel's depth, +-0x200 Z-buffered, +-0xFF
  // W-buffered. Here: the polygon sits that much nearer and its pipeline
  // tests less-or-equal, so a decal at, or slightly behind, its surface
  // draws. (A decal well in front would pass too; the DS fails it. Rare.)
  float zd = z;
  if ((p.attr & 0x4000u) != 0u) zd = max(z - (wbuf ? 255.0 : 512.0), 0.0);
  // One w at every vertex (a sprite, anything drawn flat on): nothing to correct for, and the position goes out
  // with w 1. The depth is then the value itself, not the value times w over w, whose last bit differs from one
  // triangle to the next at the same depth: with the centre's depth right (above), slivers of that shadow were left.
  if (ta.w == tb.w && tb.w == tc.w) w = 1.0;
  float zc = wbuf ? (1.0 / max(zd, 1.0)) * w : (zd / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi; poly_out(p);
  {
    const uint last = p.nverts - 3u;
    v_edge = vec3(k == 0u ? 1.0 : 0.0, (!cfan && tri == last) ? (k == 1u ? 1.0 : 0.0) : 1.0, (!cfan && tri == 0u) ? (k == 2u ? 1.0 : 0.0) : 1.0);
    // A barycentric coordinate's gradient is the side across, turned, over twice the area: one value a triangle.
    vec2 ea = vec2(float(ta.fx), float(ta.fy)) / 16.0, eb = vec2(float(tb.fx), float(tb.fy)) / 16.0, ec = vec2(float(tc.fx), float(tc.fy)) / 16.0;
    float den = max(abs((eb.x - ea.x) * (ec.y - ea.y) - (eb.y - ea.y) * (ec.x - ea.x)), 1e-6);
    vec2 sa = abs(ec - eb), sb = abs(ec - ea), sc2 = abs(eb - ea);
    // Over the coordinate's change across one DS pixel (along x or y, whichever is more): under 1, an edge pixel. One
    // value a triangle, so the quotient interpolates as the coordinate does and the fragment stage needs no second varying.
    v_edge /= max(vec3(max(sa.x, sa.y), max(sb.x, sb.y), max(sc2.x, sc2.y)) / den, vec3(1e-9));
  }
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_z = z;
  v_zp = z;
}
