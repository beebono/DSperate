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
  // Save states: the rendered output the display is reading this frame (the
  // frame was rasterised from VRAM as it was at line 215, which may since
  // have changed, so it cannot be re-rendered). Loading resets everything
  // else, the texture cache included.
  template <class S> void sync_output(S& s);

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
  //
  // RING is a build-time knob (-DDS_R3D_RING=): the live tile is
  // 3 * W * RING * 2 words of colour/depth/attr plus a 256-byte stencil row
  // per line, which at RING 16 is 99 KB -- three times the 32-36 KB DraStic
  // sizes its bins to, and well past the A55's 32 KB L1D. RING 8 halves it.
  // Must be a power of two (row_of masks with it) and at least CHUNK + 2.
  //
  // 8 is the default because it measured faster on every scene: -0.30 % to
  // -1.64 % of frame time, mlbis over-budget frames -9.03 %, output
  // byte-identical. 16 remains selectable for comparison.
#ifndef DS_R3D_RING
#define DS_R3D_RING 8
#endif
  static constexpr int W = 258, RING = DS_R3D_RING, CHUNK = RING - 2, RSIZE = W * RING;
  static_assert((RING & (RING - 1)) == 0, "RING must be a power of two");
  static_assert(CHUNK >= 2, "CHUNK must leave room for the final pass lag");
  // Ring row of frame line y (-1 and 192 are the border rows); the pixel
  // address of (x, y) is row_of(y) + 1 + x, the pixel underneath RSIZE on.
  static constexpr u32 row_of(s32 y) { return static_cast<u32>((y + 1) & (RING - 1)) * W; }

  // Colour: R 0-5, G 8-13, B 16-21, A 24-28.
  // Attr: bits 0-3 edge flags (L/R/T/B), bit 4 back-facing, bits 8-12 AA
  // coverage, bit 15 fog, bits 16-21 translucent polygon id, bit 22
  // translucent, bits 24-29 opaque polygon id.
  // The second half of each buffer holds the pixel underneath (for AA).
  // Eight words of slack: the resolve works eight lanes at a time from any
  // span start, and a group that begins near x = 255 of the last ring row
  // loads and writes back (unchanged) lanes past the under plane's end.
  std::array<u32, RSIZE * 2 + 8> color_{}, depth_{}, attr_{};
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
  struct SpanBuf;
public:
  struct Shade;
private:
  struct SpanJob;
  // Resolves a whole batch: the kernel loops jobs_ itself, so its prologue
  // and the setup that depends only on the Shade are paid once per batch
  // rather than 2.4-2.9 times per span.
  using ResolveFn = void (Renderer3D::*)(const Shade&, const SpanJob*, u32);
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
    // The resolve kernel, the depth mode and whether the vector path applies,
    // all bound once here instead of re-derived on every flush -- which for a
    // polygon that does not batch is once per scanline. Same shape as
    // gather4 above; DraStic makes the equivalent choice at bin time and it
    // is the reason its flush has no indirect calls at all
    // (docs/techniques/02 s2).
    ResolveFn resolve;
    int mode;      // pick_depth_mode
    bool vec;      // the NEON resolve applies (no shadow / wireframe / blend 2)
    // Edges all fill when AA, edge marking, blended translucency or wireframe
    // is on. Built from dispcnt, polyalpha and wireframe -- all fixed for the
    // polygon -- and so decided here rather than on every scanline.
    bool always_fill;
    // Attributes uniform across the polygon, determined once during setup.
    // This avoids rechecking the same endpoints on every scanline.
    bool attrs_constant, rgb_constant;
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
    // Everything the per-scanline path needs that only changes when an edge
    // is set up. Slope::step advances dx and y and nothing else, so the slope
    // shape (negative / xmajor / increment) and the current vertex pair hold
    // for a whole run of scanlines -- refresh_edge_state recomputes these at
    // the vertex boundaries instead. This is DraStic's "decide it at bin
    // time" applied to the fill rules (docs/techniques/02 s2); the vertex
    // pointers in particular were two indirections per scanline.
    const Vertex *vcl, *vnl, *vcr, *vnr;   // vertex(vtx[cur_vl]) and friends
    s32 wcl, wnl, wcr, wnr;                // p.w[cur_vl] and friends
    s32 zcl, znl, zcr, znr;                // p.z[cur_vl] and friends
    bool nx_l, nx_r;        // negative || !xmajor
    bool px_l, px_r;        // !negative && xmajor
    bool lneg_xm;           // left.negative && left.xmajor
    bool lxm, rxm;          // xmajor
    bool same_incr;         // left.increment == right.increment
    bool l_incr0, r_incr0;  // increment == 0
    bool next_sx_differ;    // vnl->sx != vnr->sx (symmetric, so swap-safe)
  };
  std::array<Edge, 2048> edges_{};
  // Per-line active polygon set: polygons enter at their top line (buckets
  // by ytop, in list order) and leave after their last line; the active list
  // is kept in list order, which the blending rules depend on.
  std::array<u16, 2048> order_{};        // edge indices sorted by ytop, stable
  std::array<u16, 194> bucket_{};        // order_ offset where ytop == y starts (193 = end)
  std::array<u16, 2048> active_buf_[2]{};
  std::array<u16, 2048> enter_{};   // chunk's entering polygons, re-sorted into list order
  // Dense bin-local membership for large entry slices. Materialising in edge
  // order avoids the O(n log n) sort when a tile starts many polygons.
  std::array<u64, 32> enter_bits_{};
  u16* active_ = nullptr; u16* active_next_ = nullptr;
  u32 active_count_ = 0;
  std::array<bool, 192> line_touched_{};   // a polygon was active on the line (final pass needed)

  const Gpu3D* gx_ = nullptr;
  const RenderState* rs_ = nullptr;
  // The 32-entry toon table as three 6-bit byte planes, expanded once per
  // frame from rs_->toon for the vector toon / highlight stages (flush_batch).
  alignas(16) u8 toon6_[3][32] = {};
  // The eight edge-marking colours as three 6-bit byte planes (final_pass).
  alignas(16) u8 edge6_[3][16] = {};
  void expand_toon();
  const VramMap* vm_ = nullptr;
  mutable TextureCache texcache_;
  bool rendered_once_ = false;   // the colour buffer holds a rendered frame
  const VramView* texv_ = nullptr;
  const VramView* palv_ = nullptr;

  u8  tex8(u32 addr) const;
  u16 tex16(u32 addr) const;
  u16 pal16(u32 addr) const;
  // A batch is up to BATCH_PX pixels and one more span (which may itself be a
  // full 256-pixel scanline) can always be staged before the flush, so the
  // buffers hold both plus the vector slack.
  static constexpr u32 BATCH_PX = 256;
  static constexpr u32 BATCH_CAP = BATCH_PX + 256 + 16;
  struct SpanBuf {
    s32 x0;
    // Every array carries sixteen entries of slack: the stages round the span
    // length up to their vector width (four, eight or sixteen pixels) and write
    // whole vectors, so the tail of a short span runs past `n`.
    alignas(16) u32 fac[BATCH_CAP];
    alignas(16) s32 z[BATCH_CAP];
    // The pixel stages read colour as a 6-bit channel and texture coordinates
    // as s16, so the span keeps them in those widths (a third of the bytes and
    // eight pixels a vector instead of four).
    alignas(16) u8  vr[BATCH_CAP], vg[BATCH_CAP], vb[BATCH_CAP];
    alignas(16) s16 sc[BATCH_CAP], tc[BATCH_CAP];
    alignas(16) u8 pass[BATCH_CAP];      // depth pre-pass result (kern depth_candidates)
    alignas(16) u32 tcol[BATCH_CAP];     // texels for the span (textured polygons), colour15 and
    alignas(16) u32 talp[BATCH_CAP];     // 5-bit alpha, gathered once per span
    alignas(16) u32 col[BATCH_CAP];      // shaded pixel records (18-bit colour, alpha 24-28)
  };

  // One buffer per renderer, not one per call: a batch is staged into it
  // across several calls before the pixel stages run over the whole thing.
  SpanBuf spanbuf_;
  // One staged span of the polygon currently being batched. The pixel stages
  // (texel gather and shading) run once over the whole batch instead of once
  // per span, which is what DraStic's 256-pixel flush buys
  // (docs/techniques/02 s3). Everything here is what the resolve still needs
  // per span: its scanline, its candidate range, where it sits in the batch
  // buffers, and the three-part edge/fill decisions.
  struct SpanJob {
    s32 y, ca, cb; u32 off;
    s32 xdraw, lim0, lim1, lim2;
    s32 l_cov, r_cov;
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  std::array<SpanJob, 256> jobs_{};
  u32 njobs_ = 0, batch_px_ = 0;

  // One scanline's geometry and endpoint attributes: everything the
  // per-scanline path needs that is a function of the polygon and y alone,
  // with no framebuffer state in it.
  //
  // precompute_lines derives a whole run of scanlines at once, so the edge
  // walk, the fourteen interpolations, the swapped-edge decision and the fill
  // rules are one loop over the run instead of a fresh derivation inside the
  // per-scanline path. That is DraStic's structure -- its edge and span setup
  // is all per-polygon (render_polygon_setup_spans_asm_1x,
  // render_polygon_interpolate_edges, render_polygon_edge_interpolate_*) and
  // its scanline loop is pointer arithmetic against the array those produce.
  struct LineSpan {
    s32 xstart, xend;        // span endpoints, the right-edge push-left applied
    s32 wl, wr, zl, zr;      // endpoint w / z, swapped-edge order applied
    s32 al[5], ar[5];        // endpoint r g b s t, swapped-edge order applied
    s32 l_len, r_len, l_cov, r_cov;
    s32 xa, xb;              // the clipped screen range [xa, xb)
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  // render_band rasterises at most CHUNK scanlines per render_chunk call, and
  // a polygon's run inside one chunk is bounded by that.
  std::array<LineSpan, CHUNK> lines_{};
  // Walk the edges over scanlines [y0, y1) filling lines_[0 .. y1-y0), and
  // leave the edge cursors stepped past y1-1 exactly as the per-scanline path
  // left them.
  void precompute_lines(Edge& e, s32 y0, s32 y1);
  // The half of the old render_polygon_line that framebuffer state reaches:
  // the depth pre-pass, the attribute staging and the batch job.
  void stage_line(Edge& e, s32 y, const LineSpan& ls);
  u32  texture_sample(const Shade& sh, s32 s, s32 t, u32* alpha) const;
  template <bool textured> u32 shade_pixel(const Shade& sh, u32 vr, u32 vg, u32 vb, s32 s, s32 t) const;
  void plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow);
  template <int mode> bool depth_pass(u32 addr, s32 z, u32 dstattr) const;
  // One contiguous range of one span. Called only from the batch kernels and
  // from the vector kernel's scalar fallback, so it inlines into them and the
  // Shade-dependent setup lifts out of the job loop on its own.
  template <int mode, bool textured, bool aa, bool shadow> void resolve_span(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa, bool shadow> void resolve_batch(const Shade& sh, const SpanJob* jobs, u32 n);
#if DSPERATE_NEON
  // Four pixels per step; same results as resolve_span (the specification),
  // for polygons without shadow / wireframe / toon shading.
  template <int mode, bool textured, bool aa> [[gnu::always_inline]] inline void resolve_span_vec(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa> void resolve_batch_vec(const Shade& sh, const SpanJob* jobs, u32 n);
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
                  const s32* al, const s32* ar, bool with_attrs, u32 off) const;
  void span_attrs(SpanBuf& sb, s32 xstart, s32 xend, s32 ca, s32 cb, s32 wl, s32 wr, const s32* al, const s32* ar, bool attrs_constant, bool rgb_constant) const;
  void setup_left_edge(Edge& e, s32 y) const;
  void setup_right_edge(Edge& e, s32 y) const;
  void setup_polygon(Edge& e, const Polygon& p);
  void rewind_edge(Edge& e);
  // Recompute Edge's cached per-edge-segment state. Called from the two edge
  // setups (so it cannot be missed) and once per polygon for the flat case.
  void refresh_edge_state(Edge& e) const;
  void setup_shade(Shade& sh, const Polygon& p);
  // The resolve kernel for a decoded Shade (the dispatch tables live with
  // flush_batch in render3d.cpp).
  static ResolveFn select_resolve(const Shade& sh);
  // One span's three-part edge/fill walk (left edge run, interior, right
  // edge run), clipped to the range the depth pre-pass left alive. `range`
  // draws one clipped run; the batch kernels pass their own inlined body.
  //
  // always_inline, and not negotiable: left to itself GCC emits this as a
  // real call with a 112-byte frame and five register-pair saves, paid once
  // per span. Measured, that call cost 93-176 cycles a span -- 1.5-3.1 % of
  // frame across four scenes.
  template <typename Range> [[gnu::always_inline]] inline void walk_span(const SpanJob& j, Range&& range);
  void render_shadow_mask_line(Edge& e, s32 y);
  void flush_batch(const Shade& sh);
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
  // Bin cut points. The frame is split into more bins than there are workers
  // and each worker takes the next unclaimed one, so a bin that turns out
  // heavy is absorbed by the others finishing theirs early. A static split
  // cannot do that: measured per band on the device the three came out
  // 2.47 / 5.75 / 6.96 ms, a 2.8x spread, and the emulation thread waits for
  // the slowest -- 6.96 ms against the 5.06 ms an even three-way split of the
  // same 15.19 ms would have cost.
  static constexpr u32 MAX_BINS = 32;
  // How the bins are sized. Ascending is the deadline shape (small first bin,
  // large last) and is right while bins == workers, when they all start at
  // once; the others are for the binned regime, where they do not.
  // DS_R3D_SPLIT=stair|even|desc|taper.
  enum class Split { Ascending, Even, Descending, Taper };
  static Split split_mode();
  void compute_bins(u32 nbins, u32 workers);
  static u32 bin_count(u32 workers);
  u32 adaptive_workers(u32 max_workers);
  static bool threads_forced();
  static bool adapt_enabled();
  u32 workers_now_ = 0, quiet_frames_ = 0;
  s64 wait_ema_ = 0;             // averaged block time, the regime signal
  u64 wait_ns_ = 0;              // emulation thread blocked on the raster, this frame
  std::array<s32, MAX_BINS + 1> bin_y_{};
  u32 nbins_ = 0;

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
  u32 pending_bands_ = 0;             // bins in flight (0 = nothing running)
  u64 waited_bits_ = 0;
  bool async_ = std::getenv("DS_R3D_SYNC") == nullptr;

  u32  edge_count_ = 0;
  u32* out_dst_ = nullptr;                              // where final_pass writes
  std::vector<const u32*>* texels_out_ = nullptr;       // coordinator records decoded textures
  const std::vector<const u32*>* texels_in_ = nullptr;  // worker reads them back
  s32  rendered_upto_ = 0;    // lines this instance has already rasterised this frame
  u32  setup_poly_ = 0;                                 // polygon index during build_edges
  std::vector<const u32*> poly_texels_;
  std::vector<std::unique_ptr<Renderer3D>> bands_;      // workers 1..n-1 (band 0 is this)
  u64 band_ns_[8] = {};                                 // DS_PROFILE: last frame's per-band wall time
  struct Pool;
public:
  void debug_dump(FILE* f);   // DS_WATCHDOG: band hand-off state
private:
  std::unique_ptr<Pool> pool_;

  u32  fog_density(u32 addr) const;
  void final_pass(s32 y);
  void final_pass_ref(s32 y);
public:
  // tests/gpu3d_test.cpp: final_pass against final_pass_ref on random buffers; 0 when identical.
  u32  selftest_final_pass(u32 seed, u32 dispcnt);
private:
  void clear_border(s32 y);
  void clear_line(s32 y);
};

} // namespace ds::gpu
