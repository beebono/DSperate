#version 450
// Edge marking (DISP3DCNT bit 5), a pass of its own over the finished record: the draw
// subpass leaves, per pixel, the front opaque polygon's id, fog bit and DS depth in one
// word (lean.frag's o_id under DS_FF2_EDGEMARK; the rear plane's where nothing opaque was
// drawn), and a pixel on an opaque polygon's edge whose neighbour has another id and lies behind
// it takes its id's edge colour, as the DS does after drawing and before fog. The fog the
// resolve already applied to the pixel is applied again here to the edge colour.
// The record is written through its byte view so the blend unit can mix: where the game
// has anti-aliasing on, the DS gives a marked pixel half coverage and its edge blend then
// mixes the edge colour evenly with what is behind; here with the pixel's own colour (what
// is behind is not kept), which leaves the same soft line. Alpha stays the pixel's own.
//   the word: bits 0-5 polygon id, 6 the polygon's fog bit, 7 drawn, 8 a pixel of the polygon's edge
//   (the DS marks no others: not a texture's cut-out inside a polygon), 9-31 DS depth >> 1.
// Neighbours: `width` raster pixels away, one DS pixel at any scale (the default: the outline
// the game was drawn with; width 1 above 1x is the hairline melonDS's renderers draw). Every
// pixel within a DS pixel of another id's is then marked. The neighbours one raster pixel away
// were read as well, which marked a pixel beside a sliver of another id thinner than a DS pixel
// and nothing else (12 of 16819 marked pixels in Pokemon White's scene at 4x), for 3 ms of the
// G52's 4x frame.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(binding = 0) uniform highp usampler2D ids;
layout(std430, binding = 1) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { uint clear_word; uint width; uint dispcnt; } pc;
layout(location = 0) out vec4 o_col;   // r, g, b as the record's bytes (6-bit values of 255); a the share of the edge colour
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
// A neighbour's word; past the screen's edge the rear plane's.
uint tap(ivec2 p, ivec2 size) {
  if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) return pc.clear_word;
  return texelFetch(ids, p, 0).r;
}
bool behind_other(uint c, uint n) { return ((c ^ n) & 0x3Fu) != 0u && (c >> 9) < (n >> 9); }
bool edge_at(uint c, ivec2 p, ivec2 size, int d) {
  return behind_other(c, tap(p + ivec2(-d, 0), size)) || behind_other(c, tap(p + ivec2(d, 0), size)) ||
         behind_other(c, tap(p + ivec2(0, -d), size)) || behind_other(c, tap(p + ivec2(0, d), size));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(ids, 0);
  uint c = texelFetch(ids, p, 0).r;
  if ((c & 0x180u) != 0x180u) discard;   // nothing opaque here, or not a polygon's edge
  int w = int(pc.width);
  if (!edge_at(c, p, size, w)) discard;
  uint ec = ps.edge[(c & 0x3Fu) >> 3];
  uvec3 rgb = uvec3(c15_to_18(ec, 0u), c15_to_18(ec, 4u), c15_to_18(ec, 9u));
  if ((pc.dispcnt & 128u) != 0u && (pc.dispcnt & 64u) == 0u && (c & 0x40u) != 0u) {
    uint fc = ps.fog_color;
    uvec4 fogc = uvec4(c15_to_18(fc, 0u), c15_to_18(fc, 4u), c15_to_18(fc, 9u), (fc >> 16) & 0x1Fu);
    uint d = fog_density((c >> 9) << 1);
    rgb = (fogc.rgb * d + rgb * (128u - d)) >> 7;
  }
  o_col = vec4(vec3(rgb) / 255.0, (pc.dispcnt & 16u) != 0u ? 0.5 : 1.0);
}
