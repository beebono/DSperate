#version 450
// The lean path's resolve: the four samples of a pixel into the 3D layer
// record the compositor reads (RGB666 in bits 0-21, 5-bit alpha in 24-28),
// with the DS's fog applied first, per sample by the sample's depth (the DS
// fogs the finished pixel once, after blending; the polygon's fog bit rides
// in bit 0 of the sample's alpha byte). Colour is the mean of the drawn
// samples only, so an edge over an undrawn rear plane is not tinted by the
// clear colour; alpha is the coverage-weighted mean, so such an edge pixel
// carries its coverage for the compositor.
// DS_MSAA_SHARP (vk_lean.cpp, constant_id 1, 0..1): how much the coverage
// blend is pulled towards the majority surface. 0 is the plain mean above.
// Above 0 the samples split into the surface sample 0 shows (one polygon's
// samples in a pixel share their raw colour: shading is per pixel) and the
// rest, and that surface's share of the pixel goes from its coverage
// (1/4, 3/4) towards the majority's (0, 1) by the strength; a 2/2 pixel stays
// an even blend. 1: three samples of four win outright.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
// DS_SAMPLES=1: the same resolve over single-sample attachments (video.aa off).
#if DS_SAMPLES == 1
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput ms_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInput ms_depth;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform subpassInput ms_fog;
#define NSAMPLES 1
#define LOAD(a, s) subpassLoad(a)
#else
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInputMS ms_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInputMS ms_depth;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform subpassInputMS ms_fog;
#define NSAMPLES 4
#define LOAD(a, s) subpassLoad(a, s)
#endif
layout(location = 0) out uint o_word;
layout(constant_id = 1) const float ds_msaa_sharp = 0.0;
uint decode_alpha(uint ab) { return ab < 4u ? 0u : min(31u, (ab + 4u) / 8u - 1u); }   // (a + 1) * 8
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
// One sample, fogged by its own depth (the DS fogs the finished pixel once,
// after blending, by the pixel's depth). False: undrawn.
bool fogged(int s, vec4 c, bool wbuf, bool fog_on, bool fog_colour, uvec4 fogc, out uvec3 rgb, out uint a5) {
  uint ab = uint(round(c.a * 255.0));
  rgb = uvec3(0u); a5 = 0u;
  if (ab == 0u) return false;
  rgb = uvec3(round(c.rgb * 255.0));
  a5 = decode_alpha(ab);
  if (fog_on && LOAD(ms_fog, s).r > 0.5) {
    float dz = LOAD(ms_depth, s).r;
    uint z = wbuf ? uint(clamp(1.0 / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));
    uint d = fog_density(z);
    if (fog_colour) rgb = (fogc.rgb * d + rgb * (128u - d)) >> 7;
    a5 = (fogc.a * d + a5 * (128u - d)) >> 7;
  }
  return true;
}
void main() {
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  bool fog_on = (pc.f.dispcnt & 128u) != 0u, fog_colour = (pc.f.dispcnt & 64u) == 0u;
  uint fc = ps.fog_color;
  uvec4 fogc = uvec4(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u), (fc >> 16) & 0x1Fu);
#if NSAMPLES == 4
  if (ds_msaa_sharp > 0.0) {
    // Only edge pixels (samples of more than one raw colour) take this path.
    // (Its loads stay inside it: loading the samples once for both paths
    // cost the plain resolve 0.4 ms in registers.)
    vec4 c[4] = vec4[4](LOAD(ms_col, 0), LOAD(ms_col, 1), LOAD(ms_col, 2), LOAD(ms_col, 3));
    uint k0 = packUnorm4x8(c[0]);
    if (packUnorm4x8(c[1]) != k0 || packUnorm4x8(c[2]) != k0 || packUnorm4x8(c[3]) != k0) {
      // Group A: sample 0's surface; B: the rest. Colour per group is the
      // mean of its drawn samples, alpha the mean over the group (undrawn 0).
      vec3 ca = vec3(0.0), cb = vec3(0.0);
      float na = 0.0, nb = 0.0, da = 0.0, db = 0.0, sa = 0.0, sb = 0.0;
      for (int s = 0; s < 4; ++s) {
        uvec3 rgb; uint a5;
        bool drawn = fogged(s, c[s], wbuf, fog_on, fog_colour, fogc, rgb, a5);
        if (packUnorm4x8(c[s]) == k0) { na += 1.0; sa += float(a5); if (drawn) { ca += vec3(rgb); da += 1.0; } }
        else                          { nb += 1.0; sb += float(a5); if (drawn) { cb += vec3(rgb); db += 1.0; } }
      }
      if (da + db == 0.0) { o_word = 0u; return; }
      float fa = na * 0.25;
      float wa = mix(fa, fa > 0.5 ? 1.0 : fa < 0.5 ? 0.0 : 0.5, ds_msaa_sharp), wb = 1.0 - wa;
      vec3 col_a = da > 0.0 ? ca / da : vec3(0.0), col_b = db > 0.0 ? cb / db : vec3(0.0);
      vec3 col = da > 0.0 && db > 0.0 ? col_a * wa + col_b * wb : da > 0.0 ? col_a : col_b;
      float al = (na > 0.0 ? sa / na : 0.0) * wa + (nb > 0.0 ? sb / nb : 0.0) * wb;
      uvec3 m = min(uvec3(round(col)), uvec3(63u));
      o_word = m.r | (m.g << 8) | (m.b << 16) | (min(uint(round(al)), 31u) << 24);
      return;
    }
  }
#endif
  uvec3 acc = uvec3(0u);
  uint asum = 0u, n = 0u;
  for (int s = 0; s < NSAMPLES; ++s) {
    uvec3 rgb; uint a5;
    if (!fogged(s, LOAD(ms_col, s), wbuf, fog_on, fog_colour, fogc, rgb, a5)) continue;
    acc += rgb; asum += a5; ++n;
  }
  if (n == 0u) { o_word = 0u; return; }
  uvec3 m = (acc + n / 2u) / n;
  uint r = min(m.r, 63u), g = min(m.g, 63u), b = min(m.b, 63u);
  uint a5 = min((asum + uint(NSAMPLES / 2)) / uint(NSAMPLES), 31u);
  o_word = r | (g << 8) | (b << 16) | (a5 << 24);
}
