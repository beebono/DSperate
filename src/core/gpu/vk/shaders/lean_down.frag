#version 450
// The 1x record of a frame drawn above 1x (vk_lean.cpp, Impl::down): of each
// scale x scale block of the record the texel under the DS pixel's centre,
// whole (no filter: the record is packed colour and alpha).
layout(binding = 0) uniform usampler2D rec;
layout(push_constant) uniform PC { uint scale; } pc;
layout(location = 0) out uint o;
void main() { o = texelFetch(rec, ivec2(gl_FragCoord.xy) * int(pc.scale) + int(pc.scale >> 1), 0).r; }
