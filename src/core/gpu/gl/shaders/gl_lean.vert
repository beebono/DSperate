#version 310 es
// The lean path's vertex stage on GLES (lean.vert is the Vulkan original and
// has the reasoning): polygons as fans through the hardware rasteriser, one
// instance per polygon, vertices at their 28.4 positions less the
// quarter-pixel pull, each grown 1/16 px from the polygon's centroid (the
// submit puts it in vbot). DS row y draws on GL window row y (no flip), so
// gl_FragCoord is the DS coordinate and the shared span code holds.
// Depth: Z-buffered frames carry z/2^24 in the clip z; W-buffered frames
// write gl_FragDepth from v_zp (the perspective-correct w), since the GL
// depth mapping has no room for 1/w's range.
#include "gl_pull.glsl"
flat out uint v_poly;
out vec3 v_rgb;     // 9-bit vertex colour, perspective-correct
out vec2 v_st;      // 12.4 texture coordinates, perspective-correct
out float v_zp;     // DS depth, perspective-correct
void main() {
  uint pi = uint(gl_InstanceID + u_base);
  GpuPoly p = load_poly(pi);
  uint tri = uint(gl_VertexID) / 3u, k = uint(gl_VertexID) - tri * 3u;
  if (tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0.0); v_st = vec2(0.0); v_zp = 0.0; return; }
  uint vi = k == 0u ? 0u : tri + k;
  GpuVert vt = load_vert(p.first_vert + vi);
  float w = max(float(vt.w), 1.0);
  float sxf = float(vt.fx) / 16.0 - 0.25, syf = float(vt.fy) / 16.0 - 0.25;
  vec2 c = vec2(float(int(p.vbot << 16) >> 16), float(int(p.vbot) >> 16));   // the centroid, 28.4
  vec2 dv = vec2(float(vt.fx), float(vt.fy)) - c;
  float len = length(dv);
  if (len > 0.0) { sxf += dv.x / len * 0.0625; syf += dv.y / len * 0.0625; }
  float ox = 0.0, oy = 0.0;
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (sxf + ox) / 256.0 * 2.0 - 1.0, y = (syf + oy) / 192.0 * 2.0 - 1.0;
  float zd = ds_depth(vt, p.attr);
  float zc = ds_clip_z(zd, p.attr, w);
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi;
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_zp = zd;
}
