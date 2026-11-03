#version 450
// The edge cut's band pass (lean_edge.vert): one polygon edge's band. A
// pixel the edge line crosses gets the edge byte (Renderer3D::
// set_edge_export has the layout; the CPU adds the pixel's colour, see
// Gpu3D::edge_line): the share of the pixel on its centre's
// side as the coverage, the outside across the line. Either side of the
// line counts: the polygon's own pixels (its record is in the pixel), and
// the pixels just outside it where it is in front of what was drawn there,
// so the present cuts the whole line though the draw lit each pixel whole.
// Reads the draw's polygon record and depth for this pixel only (input
// attachments: on tile); writes the byte and the pipeline's depth (its own
// attachment: the class-biased value lean_edge.vert sets, nearest wins).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_ids;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInput in_depth;
layout(location = 0) flat in uint v_poly;
layout(location = 1) flat in vec4 v_line;
layout(location = 2) flat in vec2 v_in;
layout(location = 3) noperspective in float v_zt;
layout(location = 0) out uint o_byte;
// The band depth with a tie-break: two edges meeting at a vertex have the
// same depth in that pixel, so the one crossing nearer the pixel's centre
// is set a little nearer (2^-16 of the class's third at most), and the
// list's order never decides. Writing it here costs the early test only on
// this small pass.
layout(depth_greater) out float gl_FragDepth;

float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

void main() {
  vec2 c = gl_FragCoord.xy;   // the pixel's centre
  vec2 a = v_line.xy, b = v_line.zw, d = b - a;
  bool inside = cross2(d, c - a) * cross2(d, v_in - a) > 0.0;
  uint owner = subpassLoad(in_ids).x;
  if (owner == v_poly + 1u) {
    if (!inside) discard;
  } else {
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
  if (t <= 0.0 || t >= 1.0) discard;
  // Only where that crossing lies on the edge itself: the band reaches a
  // pixel past each end, and the line extended past its vertex would write
  // over the next edge's pixels (a seam between two faces of a hill meeting
  // its crest put its byte on the sea pixel there, cutting it the wrong way).
  vec2 pcross = vert ? vec2(c.x, l) : vec2(l, c.y);
  float s = dot(pcross - a, d) / dot(d, d), sm = 0.05 / length(d);
  if (s < -sm || s > 1.0 + sm) discard;
  gl_FragDepth = gl_FragCoord.z + abs(t - 0.5) * (1.0 / 65536.0 / 3.0);
  bool neg = t < 0.5;            // the centre on the positive side: the outside is the negative neighbour
  float share = neg ? 1.0 - t : t;
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  if (cov >= 31) discard;
  o_byte = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
}
