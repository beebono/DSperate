// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <functional>
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/texcache.h"

#include <array>
#include <memory>
#include <vector>

namespace ds { struct NDS; }

namespace ds::gpu {

class Gpu3D;
struct Polygon;
struct Vertex;
struct RenderState;
class VramMap;
struct VramView;

// Software rasteriser for the 3D engine's polygon list.
//
// Scanline renderer with the hardware's fixed-point edge stepping (18-bit
// slope fraction computed as x * (1/y)), its two-stage perspective-correct
// interpolation (a 9-bit factor along Y, 8-bit along X, then linear
// interpolation of the attributes by that factor), its edge fill rules, a
// two-deep pixel stack for anti-aliasing and the final pass (edge marking,
// fog, AA blend). The behaviour follows the documentation and software
// renderer of the melonDS project (GPLv3); output is checked against it
// pixel for pixel.
//
// The working buffers are a ring of four 258-pixel lines (a one-pixel
// border each side so edge marking never tests outside them): line y is
// rendered, then the final pass of line y-1 reads lines y-2..y, and the
// finished line is copied to the output buffer. The whole working set
// (colour, depth, attributes, two pixels deep) stays in L1.
class Renderer3D {
public:
  explicit Renderer3D(NDS& nds);
  ~Renderer3D();
  void reset();

  // Rasterise the frame latched by the geometry engine.
  void render(const Gpu3D& gx);

  // One output line: RGB666 in bits 0-21, 5-bit alpha in bits 24-28.
  const u32* raw_line(u32 y) const { return &out_[y * 256]; }

  // Portable pixel-pipeline pieces, exposed for the unit tests.
  static u32 alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha);

private:
  NDS& nds_;
  // The ring holds a whole chunk of scanlines at once, not just the line
  // being drawn: polygons are rasterised chunk at a time in list order
  // (render_chunk), so every line of the chunk must be writable while any
  // polygon in it is being drawn. CHUNK lines, plus one border line either
  // side for the final pass, rounded up to a power of two for the masking.
  static constexpr int W = 258, CHUNK = 14, RING = 16, RSIZE = W * RING;
  // Ring row of frame line y (-1 and 192 are the border rows); the pixel
  // address of (x, y) is row_of(y) + 1 + x, the pixel underneath RSIZE on.
  static constexpr u32 row_of(s32 y) { return static_cast<u32>((y + 1) & (RING - 1)) * W; }

  // Colour: R 0-5, G 8-13, B 16-21, A 24-28.
  // Attr: bits 0-3 edge flags (L/R/T/B), bit 4 back-facing, bits 8-12 AA
  // coverage, bit 15 fog, bits 16-21 translucent polygon id, bit 22
  // translucent, bits 24-29 opaque polygon id.
  // The second half of each buffer holds the pixel underneath (for AA).
  std::array<u32, RSIZE * 2> color_{}, depth_{}, attr_{};
  std::array<u32, 256 * 192> out_{};   // finished lines
  std::array<u8, 256 * RING> stencil_{};   // one row per ring line: see render_chunk
  // "the polygon drawn immediately before this one on THIS line was a shadow
  // mask", which is what decides whether the stencil row is cleared or added
  // to. Per ring line, not global: render_chunk draws a polygon's whole run of
  // lines before moving to the next polygon, so a single flag would carry one
  // line's state onto the next.
  std::array<bool, RING> prev_shadow_mask_{};

  // Perspective-correct interpolation factor between two endpoints.
  template <int dir> struct Interp {
    s32 x0 = 0, x1 = 0, xdiff = 0, x = 0;
    int shift = 0; bool linear = false, wbuffer = false;
    s32 xrecip_z = 0; s32 w0n = 0, w0d = 0, w1d = 0; u32 yfactor = 0;
    u32 recip = 0;   // ceil(2^32 / xdiff) for xdiff >= 2: exact linear division by one multiply and a fix-up
    void setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf);
    void set_x(s32 xv);
    s32 interpolate(s32 y0, s32 y1) const;
    s32 interpolate_z(s32 z0, s32 z1) const;
  };

  // Everything the per-pixel work needs from the polygon and the render
  // state, decoded once per polygon.
public:
  // Everything a span needs from its polygon, decoded once (the NEON gather
  // helpers in render3d.cpp take it, hence public).
  struct Shade {
    u32 blendmode, polyalpha, polyattr;
    bool highlight, textured, wireframe, shadow, polyattr_z;   // polyattr_z: translucent pixels update depth
    u32 dispcnt, alpha_ref;
    const u16* toon;
    // Texture: format, VRAM base, size, wrap/flip, transparent-colour-0 alpha, palette base.
    u32 fmt, base, texpal, alpha0;
    s32 width, height;
    bool srep, sflip, trep, tflip;
    // Direct host pointers when the whole texture / palette lies in directly
    // mapped, host-contiguous VRAM (nullptr: go through the views).
    const u8* tex_ptr;
    const u16* pal_ptr;
    // Views for the formats that address VRAM per texel (the compressed one).
    const VramView* texv; const VramView* palv; const VramMap* vm;
    // Decoded texels from the texture cache (width*height words, colour16 |
    // alpha << 16), or nullptr when the cache is off.
    const u32* texels;
    // NEON builds: the four-texel gather specialised for (format, S wrap,
    // T wrap), or nullptr for the per-lane sampler (render3d.cpp).
    const void* gather4;
  };
private:

  // One polygon edge walked down the scanlines.
  template <int side> struct Slope {
    s32 increment = 0; bool negative = false, xmajor = false;
    Interp<1> interp;
    s32 x0 = 0, xmin = 0, xmax = 0, xlen = 0, ylen = 0, dx = 0, y = 0, xcov_incr = 0;
    s32 setup_dummy(s32 x0_, bool wbuf);
    s32 setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf);
    s32 step();
    s32 xval() const;
    template <bool swapped> void edge_params(s32* length, s32* coverage) const;
  };

  struct Edge {
    const Polygon* poly;
    Slope<0> left; Slope<1> right;
    s32 xl, xr;
    u32 cur_vl, cur_vr, next_vl, next_vr;
    Shade sh;
  };
  std::array<Edge, 2048> edges_{};
  // Per-line active polygon set: polygons enter at their top line (buckets
  // by ytop, in list order) and leave after their last line; the active list
  // is kept in list order, which the blending rules depend on.
  std::array<u16, 2048> order_{};        // edge indices sorted by ytop, stable
  std::array<u16, 194> bucket_{};        // order_ offset where ytop == y starts (193 = end)
  std::array<u16, 2048> active_buf_[2]{};
  std::array<u16, 2048> enter_{};   // chunk's entering polygons, re-sorted into list order
  u16* active_ = nullptr; u16* active_next_ = nullptr;
  u32 active_count_ = 0;
  std::array<bool, 192> line_touched_{};   // a polygon was active on the line (final pass needed)

  const Gpu3D* gx_ = nullptr;
  const RenderState* rs_ = nullptr;
  const VramMap* vm_ = nullptr;
  mutable TextureCache texcache_;
  bool rendered_once_ = false;   // the colour buffer holds a rendered frame
  const VramView* texv_ = nullptr;
  const VramView* palv_ = nullptr;

  u8  tex8(u32 addr) const;
  u16 tex16(u32 addr) const;
  u16 pal16(u32 addr) const;
  struct SpanBuf;
  u32  texture_sample(const Shade& sh, s32 s, s32 t, u32* alpha) const;
  template <bool textured> u32 shade_pixel(const Shade& sh, u32 vr, u32 vg, u32 vb, s32 s, s32 t) const;
  void plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow);
  template <int mode> bool depth_pass(u32 addr, s32 z, u32 dstattr) const;
  template <int mode, bool textured, bool aa, bool shadow> void resolve_span(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
#if DSPERATE_NEON
  // Four pixels per step; same results as resolve_span (the specification),
  // for polygons without shadow / wireframe / toon shading.
  template <int mode, bool textured, bool aa> void resolve_span_vec(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  void texture_gather4(const Shade& sh, const s16* sa, const s16* ta, u32* colour, u32* alpha) const;
  // A textured span's texels, gathered once into the span buffer before the
  // resolve loop reads them (removes an indirect call per four pixels).
  void span_texels(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;
  // The span's shaded colours (texture blend and the packed 18-bit record),
  // computed before the resolve loop so the blend mode is decided once.
  template <bool textured> void span_shade(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;
  static const void* select_gather4(const Shade& sh);
#endif
  void span_stage(SpanBuf& sb, s32 xstart, s32 xend, s32 xa, s32 xb, s32 wl, s32 wr, s32 zl, s32 zr, bool wbuffer,
                  const s32* al, const s32* ar, bool with_attrs) const;
  void span_attrs(SpanBuf& sb, s32 xstart, s32 xend, s32 ca, s32 cb, s32 wl, s32 wr, const s32* al, const s32* ar) const;
  void setup_left_edge(Edge& e, s32 y) const;
  void setup_right_edge(Edge& e, s32 y) const;
  void setup_polygon(Edge& e, const Polygon& p);
  void setup_shade(Shade& sh, const Polygon& p);
  void render_shadow_mask_line(Edge& e, s32 y);
  void render_polygon_line(Edge& e, s32 y);
  // Rasterise lines [ya, yb) polygon at a time rather than line at a time:
  // the active set for the whole chunk is merged once, then each polygon
  // draws every line it covers inside the chunk before the next one starts.
  // Per pixel the order is unchanged -- a pixel belongs to one line, and the
  // polygons still reach it in list order -- which is what the blend and
  // stencil rules depend on.
  void render_chunk(s32 ya, s32 yb);

  // ---- banded parallel rasterising ---------------------------------------
  //
  // The screen is split into horizontal bands, one worker per band. A band
  // owns its line ring, its active polygon set and its edge cursors, and
  // writes only its own output lines, so the workers share nothing mutable:
  // the only shared state is the decoded-texture cache, which is resolved to
  // plain pointers on the calling thread before any worker starts (the cache
  // itself is not thread-safe).
  //
  // Slope::setup takes the line to position at and computes the edge state
  // directly from it, so a band can enter a polygon that began above its
  // first line without walking the lines in between. The final pass of a
  // line reads its two neighbours, so a band rasterises one line above the
  // range it emits; the line below comes from its own loop.
  //
  // Band boundaries are fixed for a given band count, so the split is
  // deterministic and the output does not depend on thread scheduling.
  void build_edges(const Gpu3D& gx);
  void seed_active(s32 y);
  void render_band(s32 y0, s32 y1, u32* dst);
  void prepare_worker(const Gpu3D& gx, const std::vector<const u32*>* texels);
  static u32 band_count(u32 polygons);
  // Where to cut the screen into bands. Equal thirds of Y left band 2 with
  // 1.85x band 0's polygon lines on SM64DS, and the frame waits for the
  // slowest band, so the cut follows the work instead.
  void compute_bands(u32 nb);
  std::array<s32, 9> band_y_{};

public:
  // The raster runs on the workers while the emulation thread carries on.
  // sync_line waits for the one band that owns a display line; sync_all waits
  // for all of them and is the escape hatch for anything that would change
  // what the workers read.
  void sync_line(s32 y);
  void sync_all();
  bool raster_pending() const { return pending_bands_ != 0; }
private:
  std::function<void(u32)> job_fn_;   // outlives the dispatch, unlike a local
  u32 pending_bands_ = 0;             // bands in flight (0 = nothing running)
  u32 waited_bits_ = 0;
  bool async_ = std::getenv("DS_R3D_SYNC") == nullptr;

  u32  edge_count_ = 0;
  u32* out_dst_ = nullptr;                              // where final_pass writes
  std::vector<const u32*>* texels_out_ = nullptr;       // coordinator records decoded textures
  const std::vector<const u32*>* texels_in_ = nullptr;  // worker reads them back
  u32  setup_poly_ = 0;                                 // polygon index during build_edges
  std::vector<const u32*> poly_texels_;
  std::vector<std::unique_ptr<Renderer3D>> bands_;      // workers 1..n-1 (band 0 is this)
  u64 band_ns_[8] = {};                                 // DS_PROFILE: last frame's per-band wall time
  struct Pool;
  std::unique_ptr<Pool> pool_;

  u32  fog_density(u32 addr) const;
  void final_pass(s32 y);
  void clear_border(s32 y);
  void clear_line(s32 y);
};

} // namespace ds::gpu
