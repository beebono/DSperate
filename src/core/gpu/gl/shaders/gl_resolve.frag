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
void main() {
  ivec2 xy = ivec2(gl_FragCoord.xy);
  bool fog_on = (pc.f.dispcnt & 128u) != 0u, fog_colour = (pc.f.dispcnt & 64u) == 0u;
  vec4 c = texelFetch(s_col, xy, 0);
  uint ab = uint(round(c.a * 255.0));
  if (ab == 0u) { o_word = vec4(0.0); return; }
  uvec3 rgb = uvec3(round(c.rgb * 255.0));
  uint a5 = decode_alpha(ab);
#ifdef DS_EDGE_AA
  vec4 fg = texelFetch(s_fog, xy, 0);   // the fog bit, the edge byte, the polygon's id (gl_lean.frag)
  // Edge anti-aliasing: an edge pixel takes its neighbour across the edge for the part of it the polygon does
  // not cover. Nothing drawn there: the pixel's alpha is its coverage, and the 2D layers behind show through
  // (as the DS's own does). The same object's id there: left as it is. One texel more a pixel, two more on an edge.
  uint ecode = uint(round(fg.g * 255.0));
  if (ecode != 255u) {
    uint dir = ecode >> 6;
    float cov = 0.5 + float(ecode & 63u) * (1.0 / 126.0);
    ivec2 nxy = xy + ivec2(dir == 0u ? 1 : dir == 1u ? -1 : 0, dir == 2u ? 1 : dir == 3u ? -1 : 0);
    if (nxy.x >= 0 && nxy.y >= 0 && nxy.x < 256 * int(max(pc.f.scale, 1u)) && nxy.y < 192 * int(max(pc.f.scale, 1u))) {
      vec4 nc = texelFetch(s_col, nxy, 0);
      if (nc.a * 255.0 < 0.5) a5 = max(uint(float(a5) * cov + 0.5), 1u);
      else if (abs(texelFetch(s_fog, nxy, 0).b - fg.b) > 0.002) rgb = uvec3(mix(round(nc.rgb * 255.0), vec3(rgb), cov) + 0.5);
    }
  }
  if (fog_on && fg.r > 0.5) {
#else
  if (fog_on && texelFetch(s_fog, xy, 0).r > 0.5) {
#endif
    uint fc = ps.fog_color;
    uint fr = c15_to_18(fc, 0u), fg = c15_to_18(fc, 4u), fb = c15_to_18(fc, 9u), fa = (fc >> 16) & 0x1Fu;
    float dz = texelFetch(s_depth, xy, 0).r;
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    uint z = (wbuf && DS_WCLIP != 0) ? uint(clamp(uintBitsToFloat(pc.f.wscale) / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));   // the DS depth / 2^24 (gl_lean.vert), or 1/w under DS_GL_WCLIP
    uint d = fog_density(z);
    if (fog_colour) rgb = (uvec3(fr, fg, fb) * d + rgb * (128u - d)) >> 7;
    a5 = (fa * d + a5 * (128u - d)) >> 7;
  }
  uint r = min(rgb.r, 63u), g = min(rgb.g, 63u), b = min(rgb.b, 63u);
  o_word = vec4(float(r), float(g), float(b), float(min(a5, 31u))) / 255.0;
}
