#version 450
// The edge cut's band pass (lean_edge.vert): one polygon edge's band. A
// pixel the edge line crosses gets the edge byte (Renderer3D::
// set_edge_export has the layout; the CPU adds the pixel's colour, see
// Gpu3D::edge_line): the share of the pixel on its centre's
// side as the coverage, the outside across the line. Either side of the
// line counts: the polygon's own pixels (its record is in the pixel), and
// the pixels just outside it where it is in front of what was drawn there,
// so the present cuts the whole line though the draw lit each pixel whole.
// Reads the draw's polygon record and depth for this pixel (input
// attachments), and for a seam the record of the pixel across the line
// (the same image sampled; its own render pass, so a neighbour is fine);
// writes the byte and the pipeline's depth (its own attachment: the
// class-biased value lean_edge.vert sets, nearest wins).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_ids;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInput in_depth;
layout(set = 1, binding = 2) uniform usampler2D ids_tex;   // the record again, for the neighbour
layout(location = 0) flat in uint v_poly;
layout(location = 1) flat in vec4 v_line;
layout(location = 2) flat in vec2 v_in;
layout(location = 3) noperspective in float v_zt;
layout(location = 4) flat in uint v_seam;
layout(location = 0) out uint o_byte;
// The band depth with a tie-break: two edges meeting at a vertex have the
// same depth in that pixel, so the one crossing nearer the pixel's centre
// is set a little nearer (2^-16 of the class's third at most), and the
// order drawn never decides. (Measured free on the Mali.)
layout(depth_greater) out float gl_FragDepth;

float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
vec2 vpos(GpuVert v) { return vec2(float(v.fx), float(v.fy)) / 16.0 - 0.25; }   // as lean_edge.vert

// Whether the pixel's record `rec` (polygon index plus one) is one of the
// polygons along this seam: this one, the partner the seam table paired it
// with, else any polygon with an edge on the same two points (an edge three
// or more polygons share has one partner recorded, whichever came last,
// and which that is must not matter).
bool on_seam(uint rec) {
  if (rec == 0u) return false;
  if (rec == v_poly + 1u || rec == v_seam) return true;
  GpuPoly p = polys[rec - 1u];
  if (p.nverts < 3u || p.nverts > 10u) return false;
  vec2 prev = vpos(verts[p.first_vert + p.nverts - 1u]);
  for (uint i = 0u; i < p.nverts; ++i) {
    vec2 cur = vpos(verts[p.first_vert + i]);
    if ((prev == v_line.xy && cur == v_line.zw) || (prev == v_line.zw && cur == v_line.xy)) return true;
    prev = cur;
  }
  return false;
}

void main() {
  vec2 c = gl_FragCoord.xy;   // the pixel's centre
  vec2 a = v_line.xy, b = v_line.zw, d = b - a;
  uint owner = subpassLoad(in_ids).x;
  bool own = owner == v_poly + 1u;
  // The polygon's own pixel: the line crosses it, whichever side its centre
  // is on (the fill rules give a polygon pixels whose centre lies just
  // outside an edge; discarding those left the pixel whole, a tooth of the
  // polygon's colour past its line, every few pixels along a shallow crest).
  if (!own) {
    // Not this polygon's pixel: only where it is in front of what the pixel
    // shows (the rear plane: always). Either side of the line: a pixel whose
    // centre is just inside but which the fill rules gave to what is behind
    // (a vertex on the centre) is still crossed by this edge, and no other
    // edge describes it.
    if (owner != 0u) {
      float sd = subpassLoad(in_depth).r;
      bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
      if (!(wbuf ? v_zt > sd : v_zt < sd)) discard;
    }
  }
  // The line across the pixel's centre column (an X-major edge: the outside
  // above or below) or row, 0 at its negative side.
  bool vert = abs(d.x) >= abs(d.y);
  float l = vert ? a.y + (c.x - a.x) * d.y / d.x : a.x + (c.y - a.y) * d.x / d.y;
  float t = l - (vert ? floor(c.y) : floor(c.x));
  // A crossing on the pixel's border counts (t of 0 or 1: full or no
  // coverage): the byte keeps the present's blend band continuous there;
  // without it the band stopped dead at the pixel, a hard seam in a soft edge.
  if (t < 0.0 || t > 1.0) discard;
  // Only where that crossing lies on the edge itself: the band reaches a
  // pixel past each end, and the line extended past its vertex would write
  // over the next edge's pixels (a seam between two faces of a hill meeting
  // its crest put its byte on the sea pixel there, cutting it the wrong way).
  vec2 pcross = vert ? vec2(c.x, l) : vec2(l, c.y);
  float s = dot(pcross - a, d) / dot(d, d), sm = 0.05 / length(d);
  if (s < -sm || s > 1.0 + sm) discard;
  // The pixel's own colour stays on its owner's side of the line: the
  // polygon's side (its interior point v_in) for the polygon's own pixel,
  // the far side for a pixel of what is behind. Not the side its centre is
  // on: the two differ on the pixels above, and the byte must say where
  // the colour stays. The outside is the other side.
  float lin = vert ? a.y + (v_in.x - a.x) * d.y / d.x : a.x + (v_in.y - a.y) * d.x / d.y;
  bool poly_neg = vert ? v_in.y < lin : v_in.x < lin;   // the polygon on the negative side (above / left) of the line
  bool own_neg = own == poly_neg;
  bool neg = !own_neg;           // the outside is the negative neighbour when the owner is on the positive side
  float share = own_neg ? t : 1.0 - t;   // the owner's share of the pixel
  // A seam divides its two polygons: the byte says the pixel's colour
  // stays on one side of the line and the outside neighbour's colour fills
  // the other. Only true where this pixel and that neighbour are the two
  // polygons' own; where either is a third surface (the hill the
  // backdrop's horizon runs into, the sky a hill's face seam reaches at
  // the crest) the seam says nothing about the pixel, and the silhouette
  // there, if any, has its own byte.
  if (v_seam != 0u) {
    ivec2 q = ivec2(c) + (vert ? ivec2(0, neg ? -1 : 1) : ivec2(neg ? -1 : 1, 0));
    ivec2 sz = textureSize(ids_tex, 0);
    bool offscreen = q.x < 0 || q.y < 0 || q.x >= sz.x || q.y >= sz.y;
    if (!on_seam(owner) || (!offscreen && !on_seam(texelFetch(ids_tex, q, 0).x))) discard;
  }
  // ... and for two faces' seam crossing a pixel owned by neither (their
  // claims differ), the face on the negative side, by a smaller step still.
  gl_FragDepth = gl_FragCoord.z + abs(t - 0.5) * (1.0 / 65536.0 / 3.0) + (poly_neg ? 0.0 : 1.0 / 1048576.0 / 3.0);
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  o_byte = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
}
