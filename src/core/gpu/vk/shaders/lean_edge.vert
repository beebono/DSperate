#version 450
// The edge cut's band pass (lean_edge.frag): after the draw, every edge of
// every opaque polygon drawn again as a quad a pixel either side of the
// edge line, so the pixels the line crosses, the polygon's own and those
// just outside it, each run the line test once. One instance per polygon,
// six vertices per edge (ten edges at most). Positions are the draw's
// (lean.vert at 1x: 28.4 less the quarter-pixel pull); depth is the edge's
// own, interpolated along it, for the fragment's in-front test.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;
layout(location = 1) flat out vec4 v_line;   // the edge, a.xy and b.xy in pixels
layout(location = 2) flat out vec2 v_in;     // a point inside the polygon: its centroid (Lean::submit puts it in vbot)
layout(location = 3) noperspective out float v_zt;   // the edge's true depth (gl_FragCoord.z carries the class)
layout(location = 4) flat out uint v_seam;   // a seam: the other polygon's index plus one (vk_seams.h); 0 for a silhouette

vec2 vpos(GpuVert v) { return vec2(float(v.fx), float(v.fy)) / 16.0 - 0.25; }

void main() {
  uint pi = uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  // One draw of every edge, in its own render pass with a depth test: the
  // depth written is the edge's own in a third of the range per class
  // (seams within one DS polygon id, outlines between ids, silhouettes:
  // vk_seams.h), the highest class nearest, so a pixel two edges cross
  // keeps the higher class, else the nearer edge, else (lean_edge.frag) the
  // crossing nearer its centre: never the order drawn.
  uint vi = uint(gl_VertexIndex);
  uint e = vi / 6u, k = vi % 6u;
  v_poly = pi; v_line = vec4(0.0); v_in = vec2(0.0); v_zt = 1.0; v_seam = 0u;
  bool skip = e >= p.nverts || p.nverts < 3u || (p.flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW | DS_PF_SHADOW_MASK)) != 0u ||
              p.xmax == p.xmin || p.ybot == p.ytop;   // lines have no area to cut
  if (skip) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
  GpuVert va = verts[p.first_vert + e], vb = verts[p.first_vert + (e + 1u == p.nverts ? 0u : e + 1u)];
  vec2 a = vpos(va), b = vpos(vb), d = b - a;
  float len = length(d);
  if (len < 1e-4) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
  uint cls = (va.pad_ & 1u) == 0u ? 2u : ((va.pad_ & 2u) != 0u ? 1u : 0u);
  v_seam = cls == 2u ? 0u : (va.pad_ >> 2u);
  v_line = vec4(a, b); v_in = vec2(float(int(p.vbot << 16) >> 16), float(int(p.vbot) >> 16)) / 16.0 - 0.25;
  // Corners 0 1 2 / 2 1 3: bit 0 the b end, bit 1 the positive side.
  const uint corner[6] = uint[6](0u, 1u, 2u, 2u, 1u, 3u);
  uint cc = corner[k];
  vec2 u = d / len, n = vec2(-u.y, u.x);
  bool at_b = (cc & 1u) != 0u;
  vec2 pos = (at_b ? b + u : a - u) + n * ((cc & 2u) != 0u ? 1.0 : -1.0);
  GpuVert vt = at_b ? vb : va;
  // Depth as lean.vert has it at that vertex.
  float w = max(float(vt.w), 1.0);
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  float zd = z;
  if ((p.attr & 0x4000u) != 0u) zd = max(z - (wbuf ? 255.0 : 512.0), 0.0);
  float zc = wbuf ? (1.0 / max(zd, 1.0)) * w : (zd / 16777215.0) * w;
  v_zt = zc / w;
  gl_Position = vec4((pos.x / 256.0 * 2.0 - 1.0) * w, (pos.y / 192.0 * 2.0 - 1.0) * w, (float(2u - cls) / 3.0) * w + zc / 3.0, w);
}
