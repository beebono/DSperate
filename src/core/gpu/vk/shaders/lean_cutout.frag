#version 450
// The edge cut's cut-out pass (after the band pass, lean_edge.frag): an
// opaque polygon whose texture has transparent texels (DS_PF_TEX_ALPHA)
// drawn again over its whole area, through lean.vert. A cut-out's visible
// edge is a texel boundary where the opacity flips, and the texture
// coordinate places it at sub-pixel precision: this pixel's texel against
// its four texel neighbours; where one differs, the boundary between them
// is a line on screen (its direction from the texture coordinate's screen
// derivatives), given the band pass's axis by its slope so a run along one
// straight boundary keeps one kind and the present tilts it. A straight edge
// in the texture is a straight line on screen; a staircase drawn into the
// art stays one (Mario Kart DS's ragged sand strips). A pixel that line
// crosses gets the edge byte (Renderer3D::set_edge_export's layout) on
// either side: the polygon's own pixels (an opaque texel: its record is in
// the pixel), and those where it is transparent and in front of what shows.
// Five texel reads a fragment. (Tried on the RG DS Plus, On Tour: a
// bilinear contour sloped by fwidth, ~0.35 ms GPU but blind to half the
// pixels, its slope only valid where a 2x2 quad straddles the edge; a
// 16-texel footprint average, ~1.3 ms; a 3x3 screen-space mask, ~0.9 ms.)
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 4) readonly buffer Spans { uvec4 spans[]; };
#define DS_TEXEL_BUFFER 1
layout(binding = 2) uniform usamplerBuffer texels_tb;
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_ids;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInput in_depth;
layout(location = 0) flat in uint v_poly;
layout(location = 2) in vec2 v_st;
layout(location = 0) out uint o_byte;
#include "ds_shade.glsl"
#include "ds_spans.glsl"

// Whether texel (s, t) (whole texels) passes the alpha test, as shade_pixel
// would make its alpha (modulate or toon: texel alpha times polygon alpha).
float opaque_at(GpuPoly p, int s, int t, uint polyalpha) {
  uint ta = (sample_tex(p, s << 4, t << 4) >> 16) & 0x1Fu;
  uint a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
  return a > pc.f.alpha_ref ? 1.0 : 0.0;
}

void main() {
  GpuPoly p = polys[v_poly];
  uint polyalpha = (p.attr >> 16) & 0x1Fu, blendmode = (p.attr >> 4) & 3u;
  // Decal keeps the polygon's alpha (no cut-out); shadows are not drawn here.
  if ((p.flags & DS_PF_TEXTURED) == 0u || blendmode == 1u || blendmode == 3u) discard;
  // The draw's texture coordinate for this pixel (the span table's, as
  // lean.frag reads it; the varying where the table has no row), so the
  // texel is the one the draw alpha-tested. The slope is the varying's.
  vec2 stv = interpolateAtOffset(v_st, vec2(-0.5)) / 16.0;   // texels, at the DS's point
  vec2 dx = dFdxFine(stv), dy = dFdyFine(stv);               // per screen pixel; before any discard that could differ within a quad
  ivec3 rgb9; ivec2 st4;
  vec2 st = ds_attrs(p, true, rgb9, st4) ? (vec2(st4) + 0.01) / 16.0 : stv;
  vec2 c0 = floor(st);
  float own = opaque_at(p, int(c0.x), int(c0.y), polyalpha);
  // The nearest flip: a texel beside this one (across an s or a t
  // boundary) of the other opacity. That boundary is a line on screen whose
  // direction the derivatives give; like the band pass, an X-major line is
  // placed across the pixel's column (the outside above or below), else
  // across its row. `bestd`: the signed screen offset from the pixel's point
  // to the line along that axis.
  float bestd = 2.0;
  bool vert = false;
  for (int k = 0; k < 4; ++k) {
    bool tax = k >= 2;                                  // a t boundary (a texel row's edge)
    float sg = (k & 1) != 0 ? -1.0 : 1.0;
    vec2 cn = c0 + (tax ? vec2(0.0, sg) : vec2(sg, 0.0));
    if (opaque_at(p, int(cn.x), int(cn.y), polyalpha) == own) continue;
    float bnd = (tax ? c0.y : c0.x) + (sg > 0.0 ? 1.0 : 0.0);
    vec2 g = tax ? vec2(dx.y, dy.y) : vec2(dx.x, dy.x);  // that coordinate's screen gradient
    bool v = abs(g.y) >= abs(g.x);                       // the line (normal to g) X-major
    float gm = v ? g.y : g.x;
    if (abs(gm) < 1e-6) continue;
    float dd = (bnd - (tax ? st.y : st.x)) / gm;
    if (abs(dd) < abs(bestd)) { bestd = dd; vert = v; }
  }
  if (abs(bestd) >= 0.5) discard;   // no flip, or the line lies in the neighbour (whose own pass places it)
  bool neg = bestd < 0.0;           // the line before the point: the outside is the negative neighbour
  float best = abs(bestd);
  uint owner = subpassLoad(in_ids).x;
  bool shows = owner == v_poly + 1u;
  if (!shows && owner != 0u) {
    float sd = subpassLoad(in_depth).r;
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    if (!(wbuf ? gl_FragCoord.z > sd : gl_FragCoord.z < sd)) discard;   // behind what shows: not this polygon's edge
  }
  if ((own > 0.5) != shows) discard;   // the draw and this pass disagree on the pixel
  float tl = neg ? 0.5 - best : 0.5 + best;   // the line across the pixel, 0 at its negative side
  float share = neg ? 1.0 - tl : tl;
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  if (cov >= 31) discard;
  o_byte = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
}
