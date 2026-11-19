#version 450
// GPU present stage: one quad per view (and one for the frontend overlay),
// laid out in the logical frame and rotated onto the presented buffer. `d`
// is the logical offset inside the quad, so a fragment at logical pixel
// (rect.x + i, rect.y + j) sees d = (i + 0.5, j + 0.5).
layout(push_constant) uniform P {
  ivec4 rect;     // x, y, w, h in the logical frame
  uvec4 a;        // presented w, h; logical w, h
  uvec4 b;        // rotation (0/90/180/270), mode (0 view, 1 overlay), source base (words), view index | grid on << 8 | grid brightness << 16
  vec4 s;         // view: 256/w, 192/h, 0.5/w, 0.5/h
} pc;
layout(location = 0) out vec2 d;

void main() {
  vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  d = c * vec2(pc.rect.zw);
  vec2 l = vec2(pc.rect.xy) + d;
  float lw = float(pc.a.z), lh = float(pc.a.w);
  vec2 p;   // the inverse of Display::rotate_out's transposes, on corners
  if (pc.b.x == 90u)       p = vec2(lh - l.y, l.x);
  else if (pc.b.x == 180u) p = vec2(lw - l.x, lh - l.y);
  else if (pc.b.x == 270u) p = vec2(l.y, lw - l.x);
  else                     p = l;
  gl_Position = vec4(p / vec2(pc.a.xy) * 2.0 - 1.0, 0.0, 1.0);
}
