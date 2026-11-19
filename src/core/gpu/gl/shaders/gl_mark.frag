#version 310 es
// Edge marking on GLES (lean_mark.frag is the Vulkan original and has the reasoning): the draw
// leaves each pixel's front opaque polygon id, fog bit and DS depth in one word (gl_lean.frag's
// o_id under DS_FF2_EDGEMARK), and a pixel of an opaque polygon whose neighbour has another id
// and lies behind it takes its id's edge colour, fogged as the pixel was. The blend unit mixes
// it into the record's colour bytes: evenly where the game has anti-aliasing on.
//   the word: bits 0-5 polygon id, 6 the polygon's fog bit, 7 drawn, 8-31 DS depth.
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
uniform usampler2D s_ids;
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
uint clear_word() { return ((pc.f.clear_attr >> 24) & 0x3Fu) | ((pc.f.clear_depth & 0xFFFFFFu) << 8); }
uint tap(ivec2 p, ivec2 size) {
  if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) return clear_word();
  return texelFetch(s_ids, p, 0).r;
}
bool behind_other(uint c, uint n) { return ((c ^ n) & 0x3Fu) != 0u && (c >> 8) < (n >> 8); }
bool edge_at(uint c, ivec2 p, ivec2 size, int d) {
  return behind_other(c, tap(p + ivec2(-d, 0), size)) || behind_other(c, tap(p + ivec2(d, 0), size)) ||
         behind_other(c, tap(p + ivec2(0, -d), size)) || behind_other(c, tap(p + ivec2(0, d), size));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(s_ids, 0);
  uint c = texelFetch(s_ids, p, 0).r;
  if ((c & 0x80u) == 0u) discard;
  int w = int(max(pc.f.scale, 1u));   // one DS pixel
  if (!edge_at(c, p, size, 1) && !(w > 1 && edge_at(c, p, size, w))) discard;
  uint ec = ps.edge[(c & 0x3Fu) >> 3];
  uvec3 rgb = uvec3(c15_to_18(ec, 0u), c15_to_18(ec, 4u), c15_to_18(ec, 9u));
  if ((pc.f.dispcnt & 128u) != 0u && (pc.f.dispcnt & 64u) == 0u && (c & 0x40u) != 0u) {
    uint fc = ps.fog_color;
    uvec3 fogc = uvec3(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u));
    uint d = fog_density(c >> 8);
    rgb = (fogc * d + rgb * (128u - d)) >> 7;
  }
  o_col = vec4(vec3(rgb) / 255.0, (pc.f.flags2 & DS_FF2_GAME_AA) != 0u ? 0.5 : 1.0);
}
