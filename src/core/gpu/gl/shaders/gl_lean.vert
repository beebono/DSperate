#version 310 es
// The lean path's vertex stage on GLES (lean.vert is the Vulkan original and
// has the reasoning): polygons as fans through the hardware rasteriser, one
// instance per polygon, vertices at their 28.4 positions with their own
// attributes (lean.vert, plain). DS row y draws on GL window row y (no flip), so
// gl_FragCoord is the DS coordinate.
// Depth: Z-buffered frames carry z/2^24 in the clip z; W-buffered frames
// write gl_FragDepth from v_zp (the perspective-correct w), since the GL
// depth mapping has no room for 1/w's range.
#include "gl_pull.glsl"
flat out uint v_poly;
out vec3 v_rgb;     // 9-bit colour and 12.4 texture coordinates, perspective-correct:
out vec2 v_st;
out float v_zp;     // DS depth, perspective-correct
void main() {
  uint pi = uint(gl_InstanceID + u_base);
  GpuPoly p = load_poly(pi);
  uint tri = uint(gl_VertexID) / 3u, k = uint(gl_VertexID) - tri * 3u;
  // A polygon of four or more vertices is a fan around its centre (lean.vert has why), unless it lies
  // inside one column or row of pixels (the line case below).
  bool cfan = (pc.f.flags2 & DS_FF2_CENTRE) != 0u && p.nverts >= 4u && p.xmax != p.xmin && p.ybot != p.ytop;
  if (cfan ? tri >= p.nverts : tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0.0); v_st = vec2(0.0); v_zp = 0.0; return; }
  uint vi = k == 0u ? 0u : tri + k;
  // The fan triangle's three vertices (the attribute planes below need them all); this one is vt.
  GpuVert ta = load_vert(p.first_vert), tb = load_vert(p.first_vert + tri + 1u), tc = load_vert(p.first_vert + tri + 2u);
  if (cfan) {
    tb = load_vert(p.first_vert + tri); tc = load_vert(p.first_vert + (tri + 1u == p.nverts ? 0u : tri + 1u));
    float n = float(p.nverts), iw = 0.0, zs = 0.0;
    vec2 cp = vec2(0.0), cst = vec2(0.0); vec3 crgb = vec3(0.0);
    bool wb = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    for (uint j = 0u; j < p.nverts; ++j) {
      GpuVert q = load_vert(p.first_vert + j);
      float qw = 1.0 / max(float(q.w), 1.0);
      cp += vec2(float(q.fx), float(q.fy)); iw += qw;
      cst += vec2(float(q.s), float(q.t)) * qw; crgb += vec3(float(q.r), float(q.g), float(q.b)) * qw;
      zs += wb ? float(q.z) * qw : float(q.z);
    }
    float cw = 1.0 / iw;
    ta.fx = int(cp.x / n); ta.fy = int(cp.y / n); ta.sx = ta.fx >> 4; ta.sy = ta.fy >> 4;
    ta.w = int(n * cw + 0.5);
    ta.s = int(floor(cst.x * cw + 0.5)); ta.t = int(floor(cst.y * cw + 0.5));
    ta.r = int(crgb.r * cw + 0.5); ta.g = int(crgb.g * cw + 0.5); ta.b = int(crgb.b * cw + 0.5);
    ta.z = int(wb ? zs * cw : zs / n);
  }
  GpuVert vt = ta; if (k == 1u) vt = tb; else if (k == 2u) vt = tc;
  float w = max(float(vt.w), 1.0);
  // The game's own triangle: the 28.4 position and the vertex's own attributes, nothing moved or grown (lean.vert, plain).
  float sxf = float(vt.fx) / 16.0, syf = float(vt.fy) / 16.0;
  vec3 a_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  vec2 a_st = vec2(float(vt.s), float(vt.t));
  float x = sxf / 256.0 * 2.0 - 1.0, y = syf / 192.0 * 2.0 - 1.0;
  float zd = ds_depth(vt, p.attr);
  float zc = ds_clip_z(zd, p.attr, w);
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi;
  v_rgb = a_rgb;
  v_st = a_st;
  v_zp = zd;
}
