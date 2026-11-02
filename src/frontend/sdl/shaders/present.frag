#version 450
// GPU present stage: shades one view's quad (nearest from the core's frame,
// LCD grid seams) or the frontend overlay's. The letterbox is the render
// pass's clear; the PiP fade and the overlay are fixed-function blends.
layout(location = 0) in vec2 d;
layout(location = 0) out vec4 dst;
layout(std430, binding = 1) readonly buffer Src { uint src[]; };   // slot base + screen * 49152 + y * 256 + x, 0xAARRGGBB; the edge planes 2 * 49152 further
layout(std430, binding = 2) readonly buffer Over { uint over[]; };  // frontend overlay (OSD, menu), logical frame, pitch lw, 0xAARRGGBB
layout(std430, binding = 3) readonly buffer Tab  { uint tab[]; };   // LCD grid seam bitmasks: per view, 32 words columns then 32 rows
layout(push_constant) uniform P { ivec4 rect; uvec4 a; uvec4 b; vec4 s; } pc;   // present.vert's

// LCD grid: dims the first panel pixel/row of each source pixel/line to
// f/256. Columns/rows depend only on view size, so the CPU precomputes them
// as bitmasks and this just tests two bits a pixel.
bool grid_seam(uint v, int o, int axis) {
  return ((tab[v * 64u + uint(axis * 32 + (o >> 5))] >> uint(o & 31)) & 1u) != 0u;
}

// The panel-density edge cut (core/gpu/edge_cut.h is the reference). An
// edge plane word is the edge byte over the pixel's own unblended colour:
// bit 7 edge, bit 6 the outside is the negative neighbour (left / up), bit
// 5 the run's axis is vertical, bits 0-4 the coverage. The source pixel's
// block is cut along the edge line, its position across the axis from the
// coverage, its tilt toward the next edge pixel along the run; each panel
// pixel takes the pixel's own colour or the outside neighbour's.
float cut_pos(uint e) {
  float f = float((e & 31u) + 1u) / 32.0;
  return (e & 64u) != 0u ? 1.0 - f : f;
}
uint edge_cut(uint base, int sx, int sy, float u, float v, uint s) {
  uint eb = base + 2u * 49152u;
  uint w = src[eb + uint(sy) * 256u + uint(sx)];
  if (w == 0u) return s;
  uint e = w >> 24;
  bool vert = (e & 32u) != 0u, neg = (e & 64u) != 0u;
  float t = vert ? v : u, along = (vert ? u : v) - 0.5;
  int nx = sx + (vert ? (along < 0.0 ? -1 : 1) : 0), ny = sy + (vert ? 0 : (along < 0.0 ? -1 : 1));
  float pos = cut_pos(e), pn = pos;
  if (nx >= 0 && nx < 256 && ny >= 0 && ny < 192) { uint n = src[eb + uint(ny) * 256u + uint(nx)] >> 24; if ((n & 224u) == (e & 224u)) pn = cut_pos(n); }
  float line = pos + (pn - pos) * abs(along);
  bool inside = neg ? (t >= line) : (t < line);
  int px = vert ? sx : (neg ? sx - 1 : sx + 1), py = vert ? (neg ? sy - 1 : sy + 1) : sy;
  if (inside || px < 0 || px > 255 || py < 0 || py > 191) return 0xFF000000u | (w & 0xFFFFFFu);
  return src[base + uint(py) * 256u + uint(px)];
}

vec3 unpack(uint s) { return vec3(float((s >> 16) & 255u), float((s >> 8) & 255u), float(s & 255u)); }

void main() {
  ivec2 o = ivec2(d);
  if (pc.b.y == 1u) {
    uint lw = pc.a.z;
    uint ov = over[min(uint(pc.rect.y + o.y) * lw + uint(pc.rect.x + o.x), lw * pc.a.w - 1u)];
    dst = vec4(unpack(ov) / 255.0, float(ov >> 24) / 255.0);
    return;
  }
  // floor(o * 256 / w) exactly: the fraction is a multiple of 1/w, so half of that absorbs the float error.
  float fx = float(o.x) * pc.s.x + pc.s.z, fy = float(o.y) * pc.s.y + pc.s.w;
  int sx = min(int(fx), 255), sy = min(int(fy), 191);
  uint s = src[pc.b.z + uint(sy) * 256u + uint(sx)];
  if ((pc.b.w & 0x200u) != 0u) s = edge_cut(pc.b.z, sx, sy, clamp(fx - float(sx), 0.0, 1.0), clamp(fy - float(sy), 0.0, 1.0), s);
  vec3 col = unpack(s);
  uint gf = pc.b.w >> 16;
  if ((pc.b.w & 0x100u) != 0u && gf < 256u && (grid_seam(pc.b.w & 0xFFu, o.x, 0) || grid_seam(pc.b.w & 0xFFu, o.y, 1))) col = floor(col * (float(gf) / 256.0));
  dst = vec4(col / 255.0, 1.0);
}
