#version 450
// The lean path's last draw in the direct output (vk_lean.cpp): a fullscreen
// triangle in the draw subpass that turns the finished tile into the 3D
// layer record, in place. It reads the pixel back, fogs it as the DS does
// (once per pixel, after blending, by the pixel's depth, where its fog bit
// is set; ds_fog false: a frame that cannot fog) and writes the record's
// bytes over the colour: 6-bit channels and the 5-bit alpha itself, 0 for an
// undrawn pixel. The maths is lean_resolve.frag's, in float where every
// product is an integer below 2^14 and each divide a power of two.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInput t_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInput t_depth;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform subpassInput t_fog;
layout(location = 0) out vec4 o_col;
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
layout(constant_id = 3) const bool ds_fog = true;
layout(constant_id = 5) const bool ds_raw = false;   // the tile's alpha byte is the 5-bit alpha already (vk_lean.cpp Impl::raw), with the fog flag in bit 7
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
void main() {
  vec4 c = round(subpassLoad(t_col) * 255.0);
  bool fogged;
  if (ds_raw) { fogged = c.a >= 128.0; if (fogged) c.a -= 128.0; }   // the alpha itself, the fog flag in bit 7
  if (c.a == 0.0) { uvec3 rr; uint ra; o_col = (ds_fog && rear_fog(rr, ra)) ? vec4(vec3(rr), float(ra)) / 255.0 : vec4(0.0); return; }
  vec3 rgb = c.rgb;
  float a5 = ds_raw ? c.a : c.a < 4.0 ? 0.0 : min(31.0, floor((c.a + 4.0) * 0.125) - 1.0);   // the draw's alpha byte is (a + 1) * 8
  if (!ds_raw) fogged = subpassLoad(t_fog).r > 0.5;
  if (ds_fog && fogged) {
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    float dz = subpassLoad(t_depth).r;
    uint z = wbuf ? uint(clamp(1.0 / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));
    uint di = fog_density(z);
    if (di != 0u) {
      uint fc = ps.fog_color;
      float d = float(di), e = 128.0 - d;
      if ((pc.f.dispcnt & 64u) == 0u) rgb = floor((vec3(uvec3(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u))) * d + rgb * e) * 0.0078125);
      a5 = floor((float((fc >> 16) & 0x1Fu) * d + a5 * e) * 0.0078125);
    }
  }
  o_col = vec4(min(rgb, vec3(63.0)), min(a5, 31.0)) / 255.0;
}
