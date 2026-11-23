#version 450
// The crack fill of a frame the hardware does not resolve (vk_lean.cpp, Impl::crack; lean_msdec.frag has the rule
// and why): the frame's record is drawn into a scratch image first (the resolve subpass's output, or the direct
// output's tile) and this pass writes the record from it, a crack's pixel (ds_crack.glsl) taking the mean word of
// the two beside it (their alpha with its own colour where some of its samples were drawn). The resolve subpass says what was drawn in the word's top bits (ds_flags;
// lean_resolve.frag), from the samples before the fog; the direct output's word is the record's own, and drawn
// there is its alpha (so a crack under fog whose alpha is short of 31 stays).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(binding = 0) uniform usampler2D t_pre;
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) out uint o_word;
layout(constant_id = 2) const bool ds_flags = false;
const uint F_FULL = 0x20000000u, F_ALL = 0x40000000u, F_ANY = 0x80000000u;   // every sample at alpha 31; every sample drawn; any
bool crack_in(ivec2 q) { return q.x >= 0 && q.y >= 0 && q.x < int(256u * max(pc.f.scale, 1u)) && q.y < int(192u * max(pc.f.scale, 1u)); }
bool full_at(ivec2 q) {
  if (!crack_in(q)) return true;
  uint w = texelFetch(t_pre, q, 0).r;
  return ds_flags ? (w & F_FULL) != 0u : ((w >> 24) & 31u) == 31u;
}
bool all_at(uint w) { return ds_flags ? (w & F_ALL) != 0u : ((w >> 24) & 31u) != 0u; }
bool crack_full(ivec2 q) { return full_at(q); }
bool crack_cand(ivec2 q) { return crack_in(q) && !all_at(texelFetch(t_pre, q, 0).r); }
#include "ds_crack.glsl"
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint w = texelFetch(t_pre, p, 0).r;
  int ax = all_at(w) ? 0 : crack_axis(p, crack_longest(int(max(pc.f.scale, 1u)), ds_flags && (w & F_ANY) != 0u));
  if (ax != 0) {
    ivec2 sa, sb; crack_sides(p, ax, sa, sb);
    uint q[2] = uint[2](texelFetch(t_pre, sa, 0).r, texelFetch(t_pre, sb, 0).r);
    vec4 m = vec4(0.0);
    for (int k = 0; k < 2; ++k) m += vec4(float(q[k] & 63u), float((q[k] >> 8) & 63u), float((q[k] >> 16) & 63u), float((q[k] >> 24) & 31u));
    m = floor(m * 0.5 + 0.5);
    uvec3 rgb = (ds_flags && (w & F_ANY) != 0u) ? uvec3(w & 63u, (w >> 8) & 63u, (w >> 16) & 63u) : uvec3(m.rgb);
    w = rgb.r | (rgb.g << 8) | (rgb.b << 16) | (uint(m.a) << 24);
  }
  o_word = w & 0x1F3F3F3Fu;
}
