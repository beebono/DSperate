#version 310 es
// The edge cut's band pass on GLES (lean_edge.frag is the original): the
// edge line placed in the pixels it crosses, the polygon record and depth
// read from the draw's attachments as textures.
uniform usampler2D s_ids;
uniform sampler2D s_depth;
flat in uint v_poly;
flat in vec4 v_line;
flat in vec2 v_in;
in float v_zp;
layout(location = 0) out vec4 o_byte;
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
void main() {
  vec2 c = gl_FragCoord.xy;
  ivec2 xy = ivec2(c);
  vec2 a = v_line.xy, b = v_line.zw, d = b - a;
  bool inside = cross2(d, c - a) * cross2(d, v_in - a) > 0.0;
  uint owner = texelFetch(s_ids, xy, 0).x;
  if (owner == v_poly + 1u) {
    if (!inside) discard;
  } else {   // not this polygon's pixel: in front of what shows, either side of the line (lean_edge.frag)
    if (owner != 0u) {
      float sd = texelFetch(s_depth, xy, 0).r;
      bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
      bool inv = wbuf && DS_WCLIP != 0;
      float md = (wbuf && !inv) ? v_zp / 16777215.0 : gl_FragCoord.z;
      if (!(inv ? md > sd : md < sd)) discard;
    }
  }
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
  bool neg = t < 0.5;
  float share = neg ? 1.0 - t : t;
  int cov = clamp(int(round(share * 32.0)) - 1, 0, 31);
  if (cov >= 31) discard;
  uint bits = 0x80u | (neg ? 0x40u : 0u) | (vert ? 0x20u : 0u) | uint(cov);
  o_byte = vec4(float(bits) / 255.0, 0.0, 0.0, 1.0);
}
