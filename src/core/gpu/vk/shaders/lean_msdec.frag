#version 450
// The record of a multisampled frame resolved by the hardware on the tile (vk_lean.cpp, Impl::mshw): the
// resolved colour, fog flag and depth into the 3D layer record's word, one pixel each. What lean_resolve.frag
// makes from the samples themselves, from their means:
// - a drawn sample's colour bytes are its 6-bit channels + 192 and an undrawn one's 0, so a resolved byte is
//   48 n + (the drawn samples' sum) / 4 for n drawn samples, and the two parts come apart (the sum is at most
//   15.75 n): the colour is the mean of the drawn samples alone, as there;
// - the alpha byte is the mean of (a + 1) * 8 over the four: the coverage-weighted alpha, as there;
// - the fog is applied once, to that mean, by sample 0's depth where half the samples or more carry the flag
//   (there: each sample by its own depth, then the mean). The same inside a surface; on a pixel two surfaces
//   share, the fog of one of them for both.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(binding = 0) uniform sampler2D t_col;
layout(binding = 1) uniform sampler2D t_fog;
layout(binding = 2) uniform sampler2D t_depth;
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) out uint o_word;
// The crack fill (vk_lean.cpp, Impl::crack; ds_crack.glsl has the test and why): a crack's pixel takes the mean
// word of the two beside it where nothing was drawn (black dots otherwise, fainter under MSAA), and their alpha
// with its own colour where some samples were. A translucent pixel has every sample drawn and is never one.
layout(constant_id = 2) const bool ds_crack = false;
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
uint decode_word(ivec2 p) {
  vec4 c = round(texelFetch(t_col, p, 0) * 255.0);
  float n = min(4.0, floor(max(c.r, max(c.g, c.b)) / 48.0));
  if (n == 0.0) { uvec3 rr; uint ra; return rear_fog(rr, ra) ? (rr.r | (rr.g << 8) | (rr.b << 16) | (ra << 24)) : 0u; }
  vec3 rgb = clamp(floor((c.rgb - 48.0 * n) * 4.0 / n + 0.5), vec3(0.0), vec3(63.0));
  float sa = clamp(floor((4.0 * c.a + n) * 0.125) - n, 0.0, 31.0 * n);   // the drawn samples' alphas, summed
  if ((pc.f.dispcnt & 128u) != 0u && texelFetch(t_fog, p, 0).r >= 0.5) {
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    float dz = texelFetch(t_depth, p, 0).r;
    uint z = wbuf ? uint(clamp(1.0 / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));
    uint di = fog_density(z);
    if (di != 0u) {
      uint fc = ps.fog_color;
      float d = float(di), e = 128.0 - d;
      if ((pc.f.dispcnt & 64u) == 0u) rgb = floor((vec3(uvec3(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u))) * d + rgb * e) * 0.0078125);
      sa = floor((float((fc >> 16) & 0x1Fu) * d + sa / n * e) * 0.0078125 + 1e-3) * n;   // each sample's, floored as the DS's
    }
  }
  uvec3 m = uvec3(min(rgb, vec3(63.0)));
  return m.r | (m.g << 8) | (m.b << 16) | (uint(min(31.0, floor((sa + 2.0) * 0.25))) << 24);
}
// The drawn samples (0-4) and whether each of them is of alpha 31, from the resolved colour (decode_word's reading).
bool crack_in(ivec2 q) { return q.x >= 0 && q.y >= 0 && q.x < int(256u * max(pc.f.scale, 1u)) && q.y < int(192u * max(pc.f.scale, 1u)); }
bool full_at(ivec2 q) {
  if (!crack_in(q)) return true;
  vec4 c = round(texelFetch(t_col, q, 0) * 255.0);
  float n = min(4.0, floor(max(c.r, max(c.g, c.b)) / 48.0));
  return n == 4.0 && floor((4.0 * c.a + n) * 0.125) - n >= 123.5;
}
float drawn_at(ivec2 q) { vec4 c = round(texelFetch(t_col, q, 0) * 255.0); return min(4.0, floor(max(c.r, max(c.g, c.b)) / 48.0)); }
bool crack_full(ivec2 q) { return full_at(q); }
bool crack_cand(ivec2 q) { return crack_in(q) && drawn_at(q) < 4.0; }
#include "ds_crack.glsl"
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint w = decode_word(p);
  if (ds_crack) {
    float n = drawn_at(p);
    int ax = n < 4.0 ? crack_axis(p, crack_longest(int(max(pc.f.scale, 1u)), n > 0.0)) : 0;
    if (ax != 0) {
      ivec2 sa, sb; crack_sides(p, ax, sa, sb);
      uint q[2] = uint[2](decode_word(sa), decode_word(sb));
      vec4 m = vec4(0.0);
      for (int k = 0; k < 2; ++k) m += vec4(float(q[k] & 63u), float((q[k] >> 8) & 63u), float((q[k] >> 16) & 63u), float((q[k] >> 24) & 31u));
      m = floor(m * 0.5 + 0.5);
      uvec3 rgb = n == 0.0 ? uvec3(m.rgb) : uvec3(w & 63u, (w >> 8) & 63u, (w >> 16) & 63u);
      w = rgb.r | (rgb.g << 8) | (rgb.b << 16) | (uint(m.a) << 24);
    }
  }
  o_word = w;
}
