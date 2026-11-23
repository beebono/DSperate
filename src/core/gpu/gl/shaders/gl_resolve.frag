#version 310 es
// The lean path's resolve on GLES (lean_resolve.frag is the original, its
// one-sample form): the drawn pixel into the 3D layer record, fog applied
// by the pixel's depth, the word's bytes as the RGBA8 attachment's channels
// (r | g << 8 | b << 16 | a5 << 24 as the record holds it).
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
uniform sampler2D s_col;
uniform sampler2D s_depth;
uniform sampler2D s_fog;
layout(location = 0) out vec4 o_word;
uint decode_alpha(uint ab) { return ab < 4u ? 0u : min(31u, (ab + 4u) / 8u - 1u); }
uint c15_to_18(uint c, uint shift) { uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu); return v != 0u ? v + 1u : v; }
uint fog_density(uint z) {
  uint id, frac;
  if (z < ps.fog_offset) { id = 0u; frac = 0u; }
  else {
    z -= ps.fog_offset;
    z = (z >> 2) << ps.fog_shift;
    id = z >> 17;
    if (id >= 32u) { id = 32u; frac = 0u; } else frac = z & 0x1FFFFu;
  }
  uint d = ((ps.density[id] * (0x20000u - frac)) + (ps.density[id + 1u] * frac)) >> 17;
  return d >= 127u ? 128u : d;
}
// The rear plane where nothing is drawn (its alpha 0): fogged all the same when its fog bit is set, by the clear
// depth, as the DS fogs every pixel of the finished picture. False: nothing to show there.
bool rear_fog(out uvec3 rgb, out uint a5) {
  rgb = uvec3(0u); a5 = 0u;
  if ((pc.f.dispcnt & 128u) == 0u || (pc.f.clear_attr & 0x8000u) == 0u) return false;
  uint d = fog_density(pc.f.clear_depth);
  uint fc = ps.fog_color;
  a5 = (((fc >> 16) & 0x1Fu) * d) >> 7;
  if (a5 == 0u) return false;
  rgb = uvec3(pc.f.clear_color & 63u, (pc.f.clear_color >> 8) & 63u, (pc.f.clear_color >> 16) & 63u);
  if ((pc.f.dispcnt & 64u) == 0u) rgb = (uvec3(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u)) * d + rgb * (128u - d)) >> 7;
  return true;
}
// The crack fill (DS_CRACK; ds_crack.glsl has the test and why): a crack's pixel, on which nothing was drawn, is
// resolved as drawn, with the mean colour of the two beside it and the fog flag and depth of one of them. A pixel
// short of 31 that was drawn (a texture's own alpha) is never one.
ivec2 frame_size() { int s = int(max(pc.f.scale, 1u)); return ivec2(256 * s, 192 * s); }
bool crack_in(ivec2 q) { ivec2 size = frame_size(); return q.x >= 0 && q.y >= 0 && q.x < size.x && q.y < size.y; }
bool full_at(ivec2 q) {
  if (!crack_in(q)) return true;
  return decode_alpha(uint(round(texelFetch(s_col, q, 0).a * 255.0))) == 31u;
}
bool crack_full(ivec2 q) { return full_at(q); }
bool crack_cand(ivec2 q) { return crack_in(q) && texelFetch(s_col, q, 0).a * 255.0 < 0.5; }
#include "ds_crack.glsl"
int crack_at(ivec2 q) {
#ifdef DS_CRACK
  return crack_cand(q) ? crack_axis(q, crack_longest(int(max(pc.f.scale, 1u)), false)) : 0;
#else
  return 0;
#endif
}
void main() {
  ivec2 xy = ivec2(gl_FragCoord.xy), fxy = xy;   // fxy: where the fog flag and depth are read
  bool fog_on = (pc.f.dispcnt & 128u) != 0u, fog_colour = (pc.f.dispcnt & 64u) == 0u;
  vec4 c = texelFetch(s_col, xy, 0);
  int ax = crack_at(xy);
  if (ax != 0) {
    ivec2 sa, sb; crack_sides(xy, ax, sa, sb);
    c = (texelFetch(s_col, sa, 0) + texelFetch(s_col, sb, 0)) * 0.5;
    c.a = 255.0 / 255.0;   // alpha 31 ((31 + 1) * 8 - 1)
    fxy = sb;
  }
  uint ab = uint(round(c.a * 255.0));
  if (ab == 0u) { uvec3 rr; uint ra; o_word = rear_fog(rr, ra) ? vec4(vec3(rr), float(ra)) / 255.0 : vec4(0.0); return; }
  uvec3 rgb = uvec3(round(c.rgb * 255.0));
  uint a5 = decode_alpha(ab);
#ifdef DS_EDGE_AA
  vec4 fg = texelFetch(s_fog, fxy, 0);   // the fog bit, the edge byte, the polygon's id (gl_lean.frag)
  // Edge anti-aliasing: an edge pixel takes its neighbour across the edge for the part of it the polygon does
  // not cover. Nothing drawn there: the pixel's alpha is its coverage, and the 2D layers behind show through
  // (as the DS's own does). The same object's id there: left as it is. One texel more a pixel, two more on an edge.
  uint ecode = uint(round(fg.g * 255.0));
  if (ecode != 255u && fxy == xy) {
    uint dir = ecode >> 6;
    float cov = 0.5 + float(ecode & 63u) * (1.0 / 126.0);
    ivec2 nxy = xy + ivec2(dir == 0u ? 1 : dir == 1u ? -1 : 0, dir == 2u ? 1 : dir == 3u ? -1 : 0);
    if (crack_in(nxy)) {
      vec4 nc = texelFetch(s_col, nxy, 0);
      if (nc.a * 255.0 < 0.5) { if (crack_at(nxy) == 0) a5 = max(uint(float(a5) * cov + 0.5), 1u); }   // a filled crack is no edge
      else if (abs(texelFetch(s_fog, nxy, 0).b - fg.b) > 0.002) rgb = uvec3(mix(round(nc.rgb * 255.0), vec3(rgb), cov) + 0.5);
    }
  }
  if (fog_on && fg.r > 0.5) {
#else
  if (fog_on && texelFetch(s_fog, fxy, 0).r > 0.5) {
#endif
    uint fc = ps.fog_color;
    uint fr = c15_to_18(fc, 0u), fg = c15_to_18(fc, 4u), fb = c15_to_18(fc, 9u), fa = (fc >> 16) & 0x1Fu;
    float dz = texelFetch(s_depth, fxy, 0).r;
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    uint z = (wbuf && DS_WCLIP != 0) ? uint(clamp(uintBitsToFloat(pc.f.wscale) / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));   // the DS depth / 2^24 (gl_lean.vert), or 1/w under DS_GL_WCLIP
    uint d = fog_density(z);
    if (fog_colour) rgb = (uvec3(fr, fg, fb) * d + rgb * (128u - d)) >> 7;
    a5 = (fa * d + a5 * (128u - d)) >> 7;
  }
  uint r = min(rgb.r, 63u), g = min(rgb.g, 63u), b = min(rgb.b, 63u);
  o_word = vec4(float(r), float(g), float(b), float(min(a5, 31u))) / 255.0;
}
