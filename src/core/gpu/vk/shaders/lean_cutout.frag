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
// One texel read a fragment; the neighbours come as flags in the word. (Tried on the RG DS Plus, On Tour: a
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
layout(location = 0) out uint o_byte;   // after the band pass's byte pass: a cut-out edge's byte wins the pixel
#include "ds_shade.glsl"
#include "ds_spans.glsl"

// Whether a texel word passes the alpha test, as shade_pixel would make its
// alpha (modulate or toon: texel alpha times polygon alpha).
float opaque_word(uint tw, uint polyalpha) {
  uint ta = (tw >> 16) & 0x1Fu;
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
  // Cheapest tests first (the derivatives above are the only thing that
  // must precede a discard). The hardware's own texel: not within two
  // texels of an alpha edge (Renderer3D::mark_alpha_edges, bit 22), and
  // the pixel is done after one read. Then hidden pixels, before the rest.
  if ((sample_tex(p, int(floor(stv.x)) << 4, int(floor(stv.y)) << 4) & 0x400000u) == 0u) discard;
  // A magnified texture: its boundaries are many pixels apart, so most of
  // the band is pixels with none near. The nearest s and t boundaries from
  // the hardware's own coordinate, along the band axis as the loop below
  // measures them; the span table's point is within about a pixel of it
  // (the DS's 8-bit span factor over the row's range), so 2.5 px covers
  // every line that could lie within 0.5 px of that point.
  {
    vec2 fr = stv - floor(stv);
    vec2 gs = vec2(dx.x, dy.x), gt = vec2(dx.y, dy.y);
    float dsn = min(fr.x, 1.0 - fr.x) / max(max(abs(gs.x), abs(gs.y)), 1e-6);
    float dtn = min(fr.y, 1.0 - fr.y) / max(max(abs(gt.x), abs(gt.y)), 1e-6);
    if (min(dsn, dtn) > 2.5) discard;
  }
  uint owner = subpassLoad(in_ids).x;
  bool shows = owner == v_poly + 1u;
  if (!shows && owner != 0u) {
    float sd = subpassLoad(in_depth).r;
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    if (!(wbuf ? gl_FragCoord.z > sd : gl_FragCoord.z < sd)) discard;   // behind what shows: not this polygon's edge
  }
  ivec3 rgb9; ivec2 st4;
  vec2 st = ds_attrs(p, true, rgb9, st4) ? (vec2(st4) + 0.01) / 16.0 : stv;
  vec2 c0 = floor(st);
  uint tw = sample_tex(p, int(c0.x) << 4, int(c0.y) << 4);
  if ((tw & 0x200000u) == 0u) discard;   // no neighbour of the other alpha (bit 21)
  float own = opaque_word(tw, polyalpha);
  if ((own > 0.5) != shows) discard;   // the draw and this pass disagree on the pixel
  // The nearest flip: a texel beside this one (across an s or a t
  // boundary) of the other opacity. That boundary is a line on screen whose
  // direction the derivatives give; like the band pass, an X-major line is
  // placed across the pixel's column (the outside above or below), else
  // across its row. `bestd`: the signed screen offset from the pixel's point
  // to the line along that axis.
  float bestd = 2.0;
  bool vert = false;
  // Which neighbours are of the other alpha: the texel word's flags
  // (Renderer3D::mark_alpha_edges, bits 23-26: +s, -s, +t, -t in the
  // texture's own space), no read. The texel here is in the polygon's
  // space: sample_tex folds a repeat and a flip, and in a mirrored tile
  // the directions swap. Across the texture's border in a clamp or flip
  // mode the neighbour is the texel itself: no boundary.
  int tw_ = int(p.tex_w), th_ = int(p.tex_h);
  bool rep_s = (p.texparam & 0x10000u) != 0u, rep_t = (p.texparam & 0x20000u) != 0u;
  bool flp_s = (p.texparam & 0x40000u) != 0u, flp_t = (p.texparam & 0x80000u) != 0u;
  ivec2 cu = ivec2(c0);
  int fx = cu.x, fy = cu.y; bool mir_s = false, mir_t = false;
  if (rep_s) { if (flp_s) { mir_s = (fx & tw_) != 0; fx &= tw_ - 1; if (mir_s) fx = tw_ - 1 - fx; } else fx &= tw_ - 1; } else fx = clamp(fx, 0, tw_ - 1);
  if (rep_t) { if (flp_t) { mir_t = (fy & th_) != 0; fy &= th_ - 1; if (mir_t) fy = th_ - 1 - fy; } else fy &= th_ - 1; } else fy = clamp(fy, 0, th_ - 1);
  for (int k = 0; k < 4; ++k) {
    bool tax = k >= 2;                                  // a t boundary (a texel row's edge)
    float sg = (k & 1) != 0 ? -1.0 : 1.0;
    int kf = (tax ? mir_t : mir_s) ? (k ^ 1) : k;       // the direction in the texture's space
    if ((tw & (0x800000u << kf)) == 0u) continue;
    bool crossing = tax ? ((kf & 1) == 0 ? fy + 1 >= th_ : fy <= 0) : ((kf & 1) == 0 ? fx + 1 >= tw_ : fx <= 0);
    if (crossing && !(tax ? (rep_t && !flp_t) : (rep_s && !flp_s))) continue;
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
  float tl = neg ? 0.5 - best : 0.5 + best;   // the line across the pixel, 0 at its negative side
  float share = neg ? 1.0 - tl : tl;
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  if (cov >= 31) discard;
  o_byte = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
}
