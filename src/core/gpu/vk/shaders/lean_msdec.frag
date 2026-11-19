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
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = round(texelFetch(t_col, p, 0) * 255.0);
  float n = min(4.0, floor(max(c.r, max(c.g, c.b)) / 48.0));
  if (n == 0.0) { o_word = 0u; return; }
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
  o_word = m.r | (m.g << 8) | (m.b << 16) | (uint(min(31.0, floor((sa + 2.0) * 0.25))) << 24);
}
