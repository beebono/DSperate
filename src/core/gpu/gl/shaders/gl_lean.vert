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
// The polygon's words the fragment stage shades by, unpacked here once a vertex (lean.vert poly_out has why: on
// the Malis every operation there is paid per pixel, and the G31 has a third of the G52's arithmetic).
flat out ivec4 v_pb;    // the texcoord range: s and t min, s and t max (a clamped axis: within the texture)
flat out ivec4 v_wrap;  // texel = min(c & mask, fold - (c & mask)): mask.st, fold.st
flat out ivec4 v_tex;   // arena offset, width, blend mode | alpha << 2 (a wireframe's 0 as 31) | fog bit << 7 | textured << 8 | wireframe << 9, the edge-marking word's low bits
void poly_out(GpuPoly p) {
  uint pa = (p.attr >> 16) & 0x1Fu;
  uint fogbit = ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1u : 0u;
  ivec2 wh = ivec2(int(p.tex_w), int(p.tex_h));
  bvec2 rep = notEqual(ivec2(p.texparam) & ivec2(0x10000, 0x20000), ivec2(0));
  bvec2 flp = bvec2(rep.x && (p.texparam & 0x40000u) != 0u, rep.y && (p.texparam & 0x80000u) != 0u);
  ivec2 lo = ivec2(int(p.row_base & 0xFFFFu), int(p.pad_ & 0xFFFFu)) - 32768, hi = ivec2(int(p.row_base >> 16), int(p.pad_ >> 16)) - 32768;
  hi = max(hi - 1, lo);   // the DS reads no pixel at a span's far end (lean.vert)
  v_pb = ivec4(mix(clamp(lo, ivec2(0), wh * 16 - 1), lo, rep), mix(clamp(hi, ivec2(0), wh * 16 - 1), hi, rep));
  v_wrap = ivec4(mix(ivec2(0xFFFF), mix(wh - 1, 2 * wh - 1, flp), rep), mix(ivec2(0xFFFFF), 2 * wh - 1, flp));
  bool wire = pa == 0u && (p.flags & (DS_PF_SHADOW | DS_PF_SHADOW_MASK)) == 0u;
  v_tex = ivec4(int(p.tex_offset), wh.x, int(((p.attr >> 4) & 3u) | ((pa == 0u ? 31u : pa) << 2) | (fogbit << 7) | ((p.flags & DS_PF_TEXTURED) != 0u ? 256u : 0u) | (wire ? 512u : 0u)),
               int(((p.attr >> 24) & 0x3Fu) | (fogbit << 6) | 0x80u));
}
void main() {
  uint pi = uint(gl_InstanceID + u_base);
  GpuPoly p = load_poly(pi);
  uint tri = uint(gl_VertexID) / 3u, k = uint(gl_VertexID) - tri * 3u;
  // A polygon of four or more vertices is a fan around its centre (lean.vert has why), unless it lies
  // inside one column or row of pixels (the line case below).
  bool cfan = (pc.f.flags2 & DS_FF2_CENTRE) != 0u && p.nverts >= 4u && p.xmax != p.xmin && p.ybot != p.ytop;
  if (cfan ? tri >= p.nverts : tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0.0); v_st = vec2(0.0); v_zp = 0.0; v_pb = ivec4(0); v_wrap = ivec4(0); v_tex = ivec4(0); return; }
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
  // A sprite at 1x (DS_PF_SPRITE, vk_layout.h): its vertices on whole pixels, and the texture read half a pixel
  // up and left, at the pixel's corner (a 128th short of it, so a coordinate on a texel's edge stays in the texel).
  vec2 st_off = vec2(0.0);
  if ((p.flags & DS_PF_SPRITE) != 0u && pc.f.scale <= 1u) {
    GpuVert q0 = load_vert(p.first_vert), q1 = load_vert(p.first_vert + 1u), q2 = load_vert(p.first_vert + 2u);
    float l1 = float((q1.sx - q0.sx) + (q1.sy - q0.sy)), l2 = float((q2.sx - q1.sx) + (q2.sy - q1.sy));
    st_off = (0.5 - 1.0 / 128.0) * (vec2(float(q1.s - q0.s), float(q1.t - q0.t)) / l1 + vec2(float(q2.s - q1.s), float(q2.t - q1.t)) / l2);
    if (cfan) { ta.fx = (q0.sx + q2.sx) * 8; ta.fy = (q0.sy + q2.sy) * 8; } else { ta.fx = ta.sx * 16; ta.fy = ta.sy * 16; }
    tb.fx = tb.sx * 16; tb.fy = tb.sy * 16; tc.fx = tc.sx * 16; tc.fy = tc.sy * 16;
  }
  GpuVert vt = ta; if (k == 1u) vt = tb; else if (k == 2u) vt = tc;
  float w = max(float(vt.w), 1.0);
  // The game's own triangle: the 28.4 position and the vertex's own attributes, nothing moved or grown (lean.vert, plain).
  float sxf = float(vt.fx) / 16.0, syf = float(vt.fy) / 16.0;
  vec3 a_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  vec2 a_st = vec2(float(vt.s), float(vt.t)) - st_off;
  float x = sxf / 256.0 * 2.0 - 1.0, y = syf / 192.0 * 2.0 - 1.0;
  float zd = ds_depth(vt, p.attr);
  float zc = ds_clip_z(zd, p.attr, w);
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi; poly_out(p);
  v_rgb = a_rgb;
  v_st = a_st;
  v_zp = zd;
}
