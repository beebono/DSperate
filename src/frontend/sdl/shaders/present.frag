#version 450
// GPU present stage: shades one view's quad (nearest from the core's frame,
// LCD grid seams) or the frontend overlay's. The letterbox is the render
// pass's clear; the PiP fade and the overlay are fixed-function blends.
layout(location = 0) in vec2 d;
layout(location = 0) out vec4 dst;
layout(std430, binding = 1) readonly buffer Src { uint src[]; };   // slot base + screen * 49152 + y * 256 + x, 0xAARRGGBB
layout(std430, binding = 2) readonly buffer Over { uint over[]; };  // frontend overlay (OSD, menu), logical frame, pitch lw, 0xAARRGGBB
layout(std430, binding = 3) readonly buffer Tab  { uint tab[]; };   // LCD grid seam bitmasks: per view, 32 words columns then 32 rows
layout(push_constant) uniform P { ivec4 rect; uvec4 a; uvec4 b; vec4 s; } pc;   // present.vert's

// LCD grid: dims the first panel pixel/row of each source pixel/line to
// f/256. Columns/rows depend only on view size, so the CPU precomputes them
// as bitmasks and this just tests two bits a pixel.
bool grid_seam(uint v, int o, int axis) {
  return ((tab[v * 64u + uint(axis * 32 + (o >> 5))] >> uint(o & 31)) & 1u) != 0u;
}

vec3 unpack(uint s) { return vec3(float((s >> 16) & 255u), float((s >> 8) & 255u), float(s & 255u)); }

// The CPU scanline scaler's filters (core/gpu/gpu.cpp emit_bilinear,
// emit_row_straddle) through the texture unit: the frames again as an image
// (a layer per screen per slot, gpu_present.cpp), filtered by the sampler,
// which blends for free where the same integer blends from `src` cost the
// Mali-G52 2-3x the nearest pass. Its weights are 8-bit, so a blend can land
// a step from the CPU's. FILTER is a specialisation constant: each pipeline
// carries only its own path.
layout(constant_id = 0) const uint FILTER = 0u;   // GpuPresent::Filter: 0 nearest, 1 bilinear, 2 seams, 3 seams in linear light
// What the pipeline's draws use (gpu_present.cpp picks the pipeline by it): 1 unused, 2 the LCD grid, 4 the
// overlay, and in the merge 8 more than one texel to a panel pixel, 16 a captured image's coordinates, 32 BG0's
// scroll, 64 a capture's 15-bit words, 128 master brightness; bits 8-10 the 3D layer's scale when it is known (0:
// asked of the image); bit 11 the merge was made in a pass before this one, at the layer's scale, and is in `merged`;
// bit 12 the merged pixel is colour-corrected here (lut()); bits 13-14, with bit 11, the filter the view takes
// `merged` through (GpuPresent::Filter: 1 bilinear, 2 seams, 3 seams in linear light, the image then an sRGB view).
// The rest is compiled out: with all of it in, both builds ran out of registers on the
// Mali-G52 and a 4x view cost 0.3 to 1.5 ms more.
layout(constant_id = 1) const uint FEAT = 255u;
layout(binding = 4) uniform sampler2DArray frame;
layout(binding = 5) uniform sampler2DArray frame_srgb;   // the same, decoded to linear before filtering

// The frame's layer of the filters' image, slot * 2 + screen, from the view's base in `src`: a slot there is
// SLOT_PLANES planes (gpu_present.cpp kSlotPlanes; the screens are its first two). It was taken as 4 after the
// 3D merge's planes made it 9: the second slot's frames then came from layers that are not there, which the
// sampler turned into one screen's last frame on both views, every other frame, under bilinear and blended seams.
const uint SLOT_PLANES = 9u;
float frame_layer() { uint q = pc.b.z / 49152u; return float((q / SLOT_PLANES) * 2u + q % SLOT_PLANES); }
uint pack8(vec3 c) { uvec3 b = uvec3(round(clamp(c, 0.0, 1.0) * 255.0)); return 0xFF000000u | (b.r << 16) | (b.g << 8) | b.b; }
vec3 encode_srgb(vec3 l) { return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l)); }

// Box-filter seams: nearest, except the panel column / row that straddles
// the boundary after source pixel sx / line sy, which takes the two sides by
// coverage. One sample a pixel either way, as bilinear: at the texel's centre
// (exactly the texel), or that far past it on a straddle, where the sampler
// weighs the two sides so (a corner, both ways). A branch around the sample
// (or the storage-buffer texel beside it) cost the Mali ~1.3 ms a frame at 4x.
// In float: every product here is an integer below 2^24, so exact.
uint seam_px(ivec2 o, int sx, int sy) {
  // The boundary after line sy, in 1/192 panel rows from this row's top; inside the row (< 192): a straddle.
  float vf = float(sy + 1) * float(pc.rect.w) - 192.0 * float(o.y);
  bool vrow = sy < 191 && vf < 192.0;
  float hf = float(sx + 1) * float(pc.rect.z) - 256.0 * float(o.x);   // likewise in 1/256 columns
  bool hcol = sx < 255 && hf < 256.0;
  // Weights as the CPU's: the next column's (256 - hf)/256, the next line's
  // floor((192 - vf) * 256 / 192)/256 (a non-integer quotient sits at least
  // 1/192 from the next integer, so the nudge only guards an exact one).
  vec2 t = vec2(float(sx) + 0.5 + (hcol ? (256.0 - hf) / 256.0 : 0.0),
                float(sy) + 0.5 + (vrow ? floor((192.0 - vf) * (256.0 / 192.0) + 0.001) / 256.0 : 0.0));
  vec3 uvl = vec3(t / vec2(256.0, 192.0), frame_layer());
  if (FILTER == 3u && (vrow || hcol)) return pack8(encode_srgb(textureLod(frame_srgb, uvl, 0.0).rgb));   // blended in linear light
  return pack8(textureLod(frame, uvl, 0.0).rgb);
}

#ifdef DS_MERGE
// The 3D merge (core/gpu/split3d.h is the reference; this variant is
// present_merge_frag.spv, built with -DDS_MERGE=1). The view's frame is a
// screen composed without a 3D layer. pc.b.y holds, above the mode byte, the
// plane of `src` with the split's word per pixel (bits 8-15: a colour C and
// the weights of min((T * ka + C * kb + round) >> shift, 63) for a 3D pixel
// T) and where the screen's line table starts, in units of 192 words (bits
// 16-30: master brightness as o * m + n >> 4, m | n << 8, and in bits 20-28
// BG0's X offset on that line, read only when bit 31 says some line has one). `layer3d` is the
// 3D record at the raster's scale (RGB666 in bits 0-21, 5-bit alpha in
// 24-28: the R32UI image through an RGBA8UI view, a byte each). A word of a
// captured image shown again may have bit 31, T is elsewhere in the layer
// (x | y << 8 in the plane two on), and bit 30, the result drops each
// channel's low bit as the 15-bit capture did. In float, every product an
// integer far below 2^24: exact, so at 1x it is the CPU composite.
layout(binding = 6) uniform usampler2D layer3d;
// The screen already merged, at the layer's scale (FEAT bit 11): gpu_present.cpp draws this shader once into an
// image of the layer's size where the view is larger, and the view takes its texels. The merge then runs on a
// texel of the layer and not on every panel pixel under it (a quarter of them for a 2x layer on a 4x view).
layout(binding = 7) uniform sampler2D merged;

// One 3D texel `p` put into the pixel: `w` its word, `s` the frame's pixel, `lt` its line's table word.
vec3 merge_texel(uint w, uint op, uint s, uvec4 p, uint lt) {
  uint a3 = p.a & 31u;
  if (a3 == 0u) return unpack(s);
  float q = ((FEAT & 64u) != 0u && (w & 0x40000000u) != 0u) ? 2.0 : 1.0;   // the capture's 5 bits a channel
  if (op == 3u) {   // the 3D pixel as it is: most of a 3D scene
    vec3 t = vec3(p.rgb);
    if ((FEAT & 64u) != 0u) t = floor(t / q) * q;
    return t * 4.0 + floor(t * 0.0625);
  }
  bool by_alpha = op == 2u;
  float ka = float(by_alpha ? a3 + 1u : (w >> 18) & 31u), kb = float(by_alpha ? 31u - a3 : (w >> 23) & 31u);
  vec3 t = vec3(p.rgb);
  vec3 c = vec3(float(w & 63u), float((w >> 6) & 63u), float((w >> 12) & 63u));
  vec3 o = min(floor((t * ka + c * kb + (by_alpha ? 16.0 : 8.0)) * (by_alpha ? 0.03125 : 0.0625)), 63.0);
  if ((FEAT & 64u) != 0u) o = floor(o / q) * q;
  if ((FEAT & 128u) != 0u) {
    uint mb = src[lt];
    o = floor((o * float(mb & 255u) + float((mb >> 8) & 4095u)) * 0.0625);
  }
  return o * 4.0 + floor(o * 0.0625);   // 6 to 8 bits, as kern::expand_colours
}

// `s`: the frame's pixel (shown where the 3D layer is hidden or empty); (fx, fy): the point in DS pixels.
// Where the layer is finer than the view (more than one texel to a panel
// pixel along an axis), the pixel is the mean of the merge at up to 4 x 4
// points spread over its own area, as far as that lies in its DS pixel
// (the frame's pixel and the word are that one's): whole ratios make it the
// box filter, and nothing of the image is skipped. The record is merged
// texel by texel, never filtered first (colour and alpha are packed).
// Colour correction of a merged pixel (FEAT bit 12): the table after the slots in `src` (gpu_present.cpp
// kLutWords), a word for each colour of 6 bits a channel, which is what the core and the merge make (a mean of
// several texels goes to the nearest). The frame of a merged screen is uploaded uncorrected: this is its only one.
const uint LUT_BASE = 2u * SLOT_PLANES * 49152u;   // kSlots * kSlotWords
vec3 lut(vec3 c) {
  uvec3 q = uvec3(clamp(c, 0.0, 255.0) + 0.5) >> 2u;
  return unpack(src[LUT_BASE + (q.r | (q.g << 6) | (q.b << 12))]);
}

// seam_px over the merged picture: the same rule between the texels of `merged`, `psc` of them a DS pixel
// (a 2D pixel is psc x psc equal texels there, so its seams fall where they did). `lin`: blended in linear light.
vec3 seam_merged(ivec2 o, int psc, bool lin) {
  vec2 sz = vec2(256.0, 192.0) * float(psc);
  // floor(o * sz / rect), the +0.5 against the division's error (the fraction is a multiple of 1/rect).
  int sx = min(int((float(o.x) * sz.x + 0.5) / float(pc.rect.z)), int(sz.x) - 1);
  int sy = min(int((float(o.y) * sz.y + 0.5) / float(pc.rect.w)), int(sz.y) - 1);
  float vf = float(sy + 1) * float(pc.rect.w) - sz.y * float(o.y);
  bool vrow = sy < int(sz.y) - 1 && vf < sz.y;
  float hf = float(sx + 1) * float(pc.rect.z) - sz.x * float(o.x);
  bool hcol = sx < int(sz.x) - 1 && hf < sz.x;
  vec2 t = vec2(float(sx) + 0.5 + (hcol ? (sz.x - hf) / sz.x : 0.0),
                float(sy) + 0.5 + (vrow ? floor((sz.y - vf) * (256.0 / sz.y) + 0.001) / 256.0 : 0.0));
  vec3 c = textureLod(merged, t / sz, 0.0).rgb;
  return round((lin ? encode_srgb(c) : c) * 255.0);
}

vec3 merge3d(uint s, int sx, int sy, float fx, float fy) {
  uint mp = ((pc.b.y >> 8) & 255u) * 49152u;
  uint at = uint(sy) * 256u + uint(sx);
  uint w = src[mp + at];
  uint op = (w >> 28) & 3u;
  if (op == 0u) return unpack(s);
  uint lt = ((pc.b.y >> 16) & 0x7FFFu) * 192u + uint(sy);
  vec2 away = vec2(0.0);   // where the 3D pixel lies from here, in whole DS pixels
  if ((FEAT & 16u) != 0u && (w & 0x80000000u) != 0u) { uint xy = src[mp + 2u * 49152u + at]; away = vec2(float(int(xy & 255u) - sx), float(int(xy >> 8) - sy)); }
  else if ((FEAT & 32u) != 0u && (pc.b.y & 0x80000000u) != 0u) {   // some line has BG0 scrolled: the 3D pixel lies that far along (9 bits, signed), or off the layer
    away.x = float(int(src[lt] << 3) >> 23);
    if (fx + away.x < 0.0 || fx + away.x >= 256.0) return unpack(s);
  }
  const int lsc = int((FEAT >> 8) & 7u);
  ivec2 ts = lsc != 0 ? ivec2(256, 192) * lsc : textureSize(layer3d, 0);
  vec2 tpd = lsc != 0 ? vec2(float(lsc)) : vec2(ts) / vec2(256.0, 192.0);   // texels a DS pixel
  ivec2 n = clamp((ts + pc.rect.zw - 1) / pc.rect.zw, ivec2(1), ivec2(4));   // texels a panel pixel, rounded up
  if ((FEAT & 8u) == 0u || n.x * n.y == 1) return merge_texel(w, op, s, texelFetch(layer3d, clamp(ivec2((vec2(fx, fy) + away) * tpd), ivec2(0), ts - 1), 0), lt);
  vec2 lo = max(vec2(fx, fy) - pc.s.zw, vec2(float(sx), float(sy)));   // the panel pixel's area within the DS pixel
  vec2 hi = min(vec2(fx, fy) - pc.s.zw + pc.s.xy, vec2(float(sx + 1), float(sy + 1)));
  vec3 sum = vec3(0.0);
  for (int j = 0; j < n.y; ++j)
    for (int i = 0; i < n.x; ++i) {
      vec2 at2 = lo + (hi - lo) * (vec2(float(i), float(j)) + 0.5) / vec2(n) + away;
      sum += merge_texel(w, op, s, texelFetch(layer3d, clamp(ivec2(at2 * tpd), ivec2(0), ts - 1), 0), lt);
    }
  return sum / float(n.x * n.y);
}
#endif

void main() {
  ivec2 o = ivec2(d);
#ifdef DS_MERGE
  if ((FEAT & 4u) != 0u && (pc.b.y & 255u) == 1u) {
#else
  if ((FEAT & 4u) != 0u && pc.b.y == 1u) {
#endif
    uint lw = pc.a.z;
    uint ov = over[min(uint(pc.rect.y + o.y) * lw + uint(pc.rect.x + o.x), lw * pc.a.w - 1u)];
    dst = vec4(unpack(ov) / 255.0, float(ov >> 24) / 255.0);
    return;
  }
  // floor(o * 256 / w) exactly: the fraction is a multiple of 1/w, so half of that absorbs the float error.
  float fx = float(o.x) * pc.s.x + pc.s.z, fy = float(o.y) * pc.s.y + pc.s.w;
  int sx = min(int(fx), 255), sy = min(int(fy), 191);
  const uint filt = FILTER;
  if (filt == 1u) { dst = vec4(textureLod(frame, vec3(d / vec2(pc.rect.zw), frame_layer()), 0.0).rgb, 1.0); return; }
  // Seams stand in for the grid on an upscaled view; smaller, it is plain nearest (as the CPU's).
  bool seams = filt >= 2u && pc.rect.w >= 192;
  vec3 col;
#ifdef DS_MERGE
  if (!seams && (pc.b.y >> 8) != 0u) {
    if ((FEAT & 0x800u) != 0u) {
      const int psc = int((FEAT >> 8) & 7u);   // the same texel merge3d would fetch
      const uint mf = (FEAT >> 13) & 3u;
      if (mf == 1u) { dst = vec4(textureLod(merged, d / vec2(pc.rect.zw), 0.0).rgb, 1.0); return; }
      if (mf >= 2u) col = seam_merged(o, psc, mf == 3u);
      else col = round(texelFetch(merged, min(ivec2(vec2(fx, fy) * float(psc)), ivec2(256, 192) * psc - 1), 0).rgb * 255.0);
    } else {
      col = merge3d(src[pc.b.z + uint(sy) * 256u + uint(sx)], sx, sy, fx, fy);
      if ((FEAT & 0x1000u) != 0u) col = lut(col);
    }
  }
  else
#endif
  col = unpack(seams ? seam_px(o, sx, sy) : src[pc.b.z + uint(sy) * 256u + uint(sx)]);
  uint gf = (pc.b.w >> 16) & 0x1FFu;
  if ((FEAT & 2u) != 0u && filt == 0u && (pc.b.w & 0x100u) != 0u && gf < 256u && (grid_seam(pc.b.w & 0xFFu, o.x, 0) || grid_seam(pc.b.w & 0xFFu, o.y, 1))) col = floor(col * (float(gf) / 256.0));
  dst = vec4(col / 255.0, 1.0);
}
