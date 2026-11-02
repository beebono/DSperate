#version 310 es
#extension GL_OES_shader_multisample_interpolation : enable
// The edge cut's cut-out pass on GLES (lean_cutout.frag is the original and
// explains the method): an alpha-tested opaque polygon drawn again over its
// area, the texel boundary where the opacity flips placed as an edge line.
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
layout(std430, binding = 4) readonly buffer Spans  { uvec4 spans[]; };
layout(std430, binding = 2) readonly buffer Texels { uint texels[]; };
layout(std430, binding = 3) readonly buffer Post   { GpuPost ps; };
uniform usampler2D s_ids;
uniform sampler2D s_depth;
flat in uint v_poly;
in vec3 v_rgb;
in vec2 v_st;
in float v_zp;
layout(location = 0) out vec4 o_byte;
#include "ds_shade.glsl"
#include "ds_spans.glsl"
#ifdef GL_OES_shader_multisample_interpolation
#define DS_AT_CORNER(v) interpolateAtOffset(v, vec2(-0.5))
#else
#define DS_AT_CORNER(v) ((v) - 0.5 * (dFdx(v) + dFdy(v)))
#endif
float opaque_word(uint tw, uint polyalpha) {
  uint ta = (tw >> 16) & 0x1Fu;
  uint a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
  return a > pc.f.alpha_ref ? 1.0 : 0.0;
}
void main() {
  GpuPoly p = polys[v_poly];
  uint polyalpha = (p.attr >> 16) & 0x1Fu, blendmode = (p.attr >> 4) & 3u;
  if ((p.flags & DS_PF_TEXTURED) == 0u || blendmode == 1u || blendmode == 3u) discard;
  vec2 stv = DS_AT_CORNER(v_st) / 16.0;
  vec2 dx = dFdx(stv), dy = dFdy(stv);
  if ((sample_tex(p, int(floor(stv.x)) << 4, int(floor(stv.y)) << 4) & 0x400000u) == 0u) discard;
  {
    vec2 fr = stv - floor(stv);
    vec2 gs = vec2(dx.x, dy.x), gt = vec2(dx.y, dy.y);
    float dsn = min(fr.x, 1.0 - fr.x) / max(max(abs(gs.x), abs(gs.y)), 1e-6);
    float dtn = min(fr.y, 1.0 - fr.y) / max(max(abs(gt.x), abs(gt.y)), 1e-6);
    if (min(dsn, dtn) > 2.5) discard;
  }
  ivec2 xy = ivec2(gl_FragCoord.xy);
  uint owner = texelFetch(s_ids, xy, 0).x;
  bool shows = owner == v_poly + 1u;
  if (!shows && owner != 0u) {
    float sd = texelFetch(s_depth, xy, 0).r;
    bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
    bool inv = wbuf && DS_WCLIP != 0;
    float md = (wbuf && !inv) ? v_zp / 16777215.0 : gl_FragCoord.z;
    if (!(inv ? md > sd : md < sd)) discard;
  }
  ivec3 rgb9; ivec2 st4;
  vec2 st = ds_attrs(p, true, rgb9, st4) ? (vec2(st4) + 0.01) / 16.0 : stv;
  vec2 c0 = floor(st);
  uint tw = sample_tex(p, int(c0.x) << 4, int(c0.y) << 4);
  if ((tw & 0x200000u) == 0u) discard;
  float own = opaque_word(tw, polyalpha);
  if ((own > 0.5) != shows) discard;
  float bestd = 2.0;
  bool vert = false;
  int tw_ = int(p.tex_w), th_ = int(p.tex_h);
  bool rep_s = (p.texparam & 0x10000u) != 0u, rep_t = (p.texparam & 0x20000u) != 0u;
  bool flp_s = (p.texparam & 0x40000u) != 0u, flp_t = (p.texparam & 0x80000u) != 0u;
  ivec2 cu = ivec2(c0);
  int fx = cu.x, fy = cu.y; bool mir_s = false, mir_t = false;
  if (rep_s) { if (flp_s) { mir_s = (fx & tw_) != 0; fx &= tw_ - 1; if (mir_s) fx = tw_ - 1 - fx; } else fx &= tw_ - 1; } else fx = clamp(fx, 0, tw_ - 1);
  if (rep_t) { if (flp_t) { mir_t = (fy & th_) != 0; fy &= th_ - 1; if (mir_t) fy = th_ - 1 - fy; } else fy &= th_ - 1; } else fy = clamp(fy, 0, th_ - 1);
  for (int k = 0; k < 4; ++k) {
    bool tax = k >= 2;
    float sg = (k & 1) != 0 ? -1.0 : 1.0;
    int kf = (tax ? mir_t : mir_s) ? (k ^ 1) : k;
    if ((tw & (0x800000u << uint(kf))) == 0u) continue;
    bool crossing = tax ? ((kf & 1) == 0 ? fy + 1 >= th_ : fy <= 0) : ((kf & 1) == 0 ? fx + 1 >= tw_ : fx <= 0);
    if (crossing && !(tax ? (rep_t && !flp_t) : (rep_s && !flp_s))) continue;
    float bnd = (tax ? c0.y : c0.x) + (sg > 0.0 ? 1.0 : 0.0);
    vec2 g = tax ? vec2(dx.y, dy.y) : vec2(dx.x, dy.x);
    bool v = abs(g.y) >= abs(g.x);
    float gm = v ? g.y : g.x;
    if (abs(gm) < 1e-6) continue;
    float dd = (bnd - (tax ? st.y : st.x)) / gm;
    if (abs(dd) < abs(bestd)) { bestd = dd; vert = v; }
  }
  if (abs(bestd) >= 0.5) discard;
  bool neg = bestd < 0.0;
  float best = abs(bestd);
  float tl = neg ? 0.5 - best : 0.5 + best;
  float share = neg ? 1.0 - tl : tl;
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  if (cov >= 31) discard;
  uint bits = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
  o_byte = vec4(float(bits) / 255.0, 0.0, 0.0, 1.0);
}
