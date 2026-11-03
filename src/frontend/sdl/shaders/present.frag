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
// Within one 6-bit step a channel (8-bit: 4): two polygons of one surface.
bool same_colour(uint a, uint b) {
  ivec3 d = abs(ivec3(int((a >> 16) & 255u), int((a >> 8) & 255u), int(a & 255u)) - ivec3(int((b >> 16) & 255u), int((b >> 8) & 255u), int(b & 255u)));
  return max(d.x, max(d.y, d.z)) <= 4;
}
float cut_pos(uint e) {
  float f = float((e & 31u) + 1u) / 32.0;
  return (e & 64u) != 0u ? 1.0 - f : f;
}
vec3 unpack(uint s) { return vec3(float((s >> 16) & 255u), float((s >> 8) & 255u), float(s & 255u)); }

// The edge line of edge byte `e` at source pixel (x, y), across the run's
// axis, at `along` (-0.5..0.5) along it: tilted toward the next edge pixel
// of the same kind on that side, else toward one whose outside faces back,
// beside it or one step across the axis toward the outside (a shallow line
// is held alternately by the polygon's pixel and the one just outside it).
float edge_line(uint eb, uint e, int x, int y, float along) {
  bool vert = (e & 32u) != 0u, neg = (e & 64u) != 0u;
  int nx = x + (vert ? (along < 0.0 ? -1 : 1) : 0), ny = y + (vert ? 0 : (along < 0.0 ? -1 : 1));
  float pos = cut_pos(e), pn = pos;
  if (nx >= 0 && nx < 256 && ny >= 0 && ny < 192) {
    uint n = src[eb + uint(ny) * 256u + uint(nx)] >> 24;
    if ((n & 224u) == (e & 224u)) pn = cut_pos(n);
    else if ((n & 224u) == ((e ^ 64u) & 224u)) pn = cut_pos(n);   // beside it on the same row, the line's other side
    else {
      int ox = nx + (vert ? 0 : (neg ? -1 : 1)), oy = ny + (vert ? (neg ? -1 : 1) : 0);
      if (ox >= 0 && ox < 256 && oy >= 0 && oy < 192) {
        uint o = src[eb + uint(oy) * 256u + uint(ox)] >> 24;
        if ((o & 224u) == ((e ^ 64u) & 224u)) pn = cut_pos(o) + (neg ? -1.0 : 1.0);
      }
    }
  }
  return pos + (pn - pos) * abs(along);
}

// The cut of panel point (u, v) in source pixel (sx, sy): the two colours
// mixed by the point's distance from the line, over a band `bw` DS pixels
// wide (1 by default, DS_EDGE_BLEND; 0: the hard cut). A band wider than the
// edge pixel's own part reaches the pixel beyond it (one with no edge of its
// own: its neighbours' are looked up), which also smooths where the line
// steps from one row of edge pixels to the next.
vec3 edge_cut(uint base, int sx, int sy, float u, float v, uint s, float bw) {
  uint eb = base + 2u * 49152u;
  uint w = src[eb + uint(sy) * 256u + uint(sx)];
  if ((w & 0x80000000u) == 0u) {   // no edge: 0, or the tag of a pixel beside an edge pixel (one neighbour lookup only for those)
    vec3 c = unpack(s);
    if (bw <= 0.0 || w == 0u) return c;
    float best = 0.0;
    const ivec2 nb[4] = ivec2[4](ivec2(0, -1), ivec2(0, 1), ivec2(-1, 0), ivec2(1, 0));
    for (int k = 0; k < 4; ++k) {
      int qx = sx + nb[k].x, qy = sy + nb[k].y;
      if (qx < 0 || qx > 255 || qy < 0 || qy > 191) continue;
      uint wn = src[eb + uint(qy) * 256u + uint(qx)];
      if ((wn & 0x80000000u) == 0u) continue;
      uint en = wn >> 24;
      bool vn = (en & 32u) != 0u, gn = (en & 64u) != 0u;
      // Only an edge whose outside is this pixel.
      bool toward = vn ? (nb[k].x == 0 && (nb[k].y == -1 ? !gn : gn)) : (nb[k].y == 0 && (nb[k].x == -1 ? !gn : gn));
      if (!toward) continue;
      uint own = 0xFF000000u | (wn & 0xFFFFFFu);
      if (same_colour(own, s)) continue;
      float tn = vn ? v - float(nb[k].y) : u - float(nb[k].x);
      float ln = edge_line(eb, en, qx, qy, (vn ? u : v) - 0.5);
      float wgt = clamp(0.5 + (gn ? tn - ln : ln - tn) / bw, 0.0, 1.0);
      if (wgt > best) { best = wgt; c = mix(unpack(s), unpack(own), wgt); }
    }
    return c;
  }
  uint e = w >> 24;
  bool vert = (e & 32u) != 0u, neg = (e & 64u) != 0u;
  float t = vert ? v : u;
  float line = edge_line(eb, e, sx, sy, (vert ? u : v) - 0.5);
  bool inside = neg ? (t >= line) : (t < line);
  int px = vert ? sx : (neg ? sx - 1 : sx + 1), py = vert ? (neg ? sy - 1 : sy + 1) : sy;
  uint own = 0xFF000000u | (w & 0xFFFFFFu);
  if (px < 0 || px > 255 || py < 0 || py > 191) return unpack(own);
  uint o = src[base + uint(py) * 256u + uint(px)];
  if (same_colour(own, o)) return unpack(s);   // a seam inside one surface: nothing to cut
  // Another line of the same axis in the outside pixel, its outside facing back: a strip between two edges that owns neither pixel (edge_cut.h); the pixel stays whole.
  { uint n = src[eb + uint(py) * 256u + uint(px)] >> 24; if ((n & 128u) != 0u && ((n ^ e) & 96u) == 64u) return unpack(s); }
  if (bw <= 0.0) return unpack(inside ? own : o);
  return mix(unpack(o), unpack(own), clamp(0.5 + (neg ? t - line : line - t) / bw, 0.0, 1.0));
}

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
  vec3 col = (pc.b.w & 0x200u) != 0u ? edge_cut(pc.b.z, sx, sy, clamp(fx - float(sx), 0.0, 1.0), clamp(fy - float(sy), 0.0, 1.0), s, float((pc.b.w >> 10) & 63u) / 16.0) : unpack(s);
  uint gf = pc.b.w >> 16;
  if ((pc.b.w & 0x100u) != 0u && gf < 256u && (grid_seam(pc.b.w & 0xFFu, o.x, 0) || grid_seam(pc.b.w & 0xFFu, o.y, 1))) col = floor(col * (float(gf) / 256.0));
  dst = vec4(col / 255.0, 1.0);
}
