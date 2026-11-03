#version 310 es
// The edge cut's band pass on GLES (lean_edge.vert is the original): every
// edge of every opaque polygon as a quad a pixel either side of the line.
#include "gl_pull.glsl"
flat out uint v_poly;
flat out vec4 v_line;   // the edge, a.xy and b.xy in pixels
flat out vec2 v_in;     // a point inside the polygon: its centroid (vbot)
out float v_zp;         // the edge's DS depth, for the fragment's in-front test
vec2 vpos(GpuVert v) { return vec2(float(v.fx), float(v.fy)) / 16.0 - 0.25; }
void main() {
  uint pi = uint(gl_InstanceID + u_base);
  GpuPoly p = load_poly(pi);
  uint vi = uint(gl_VertexID);   // 0-59 the shared edges (seams), 60-119 the rest, drawn after (lean_edge.vert)
  bool sil = vi >= 60u;
  if (sil) vi -= 60u;
  uint e = vi / 6u, k = vi - e * 6u;
  v_poly = pi; v_line = vec4(0.0); v_in = vec2(0.0); v_zp = 0.0;
  bool skip = e >= p.nverts || p.nverts < 3u || (p.flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW | DS_PF_SHADOW_MASK)) != 0u ||
              p.xmax == p.xmin || p.ybot == p.ytop;
  if (skip) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
  GpuVert va = load_vert(p.first_vert + e), vb = load_vert(p.first_vert + (e + 1u == p.nverts ? 0u : e + 1u));
  vec2 a = vpos(va), b = vpos(vb), d = b - a;
  float len = length(d);
  if (len < 1e-4 || ((va.pad_ & 1u) != 0u) == sil) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
  v_line = vec4(a, b); v_in = vec2(float(int(p.vbot << 16) >> 16), float(int(p.vbot) >> 16)) / 16.0 - 0.25;
  uint cc = k == 0u ? 0u : k == 1u ? 1u : k == 2u ? 2u : k == 3u ? 2u : k == 4u ? 1u : 3u;   // corners 0 1 2 / 2 1 3
  vec2 u = d / len, n = vec2(-u.y, u.x);
  bool at_b = (cc & 1u) != 0u;
  vec2 pos = (at_b ? b + u : a - u) + n * ((cc & 2u) != 0u ? 1.0 : -1.0);
  GpuVert vt = at_b ? vb : va;
  float w = max(float(vt.w), 1.0);
  float zd = ds_depth(vt, p.attr);
  float zc = ds_clip_z(zd, p.attr, w);
  gl_Position = vec4((pos.x / 256.0 * 2.0 - 1.0) * w, (pos.y / 192.0 * 2.0 - 1.0) * w, zc, w);
  v_zp = zd;
}
