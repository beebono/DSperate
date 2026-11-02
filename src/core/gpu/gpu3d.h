// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <algorithm>
#include <cstring>
#include "core/types.h"
#include "core/gpu/render3d.h"

#include <array>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace ds { struct NDS; }

namespace ds::gpu {

// 3D geometry engine: command FIFO, matrix stacks, lighting, clipping,
// viewport transform, and the double-buffered vertex/polygon RAM the
// rasteriser consumes. Fixed-point integer math throughout (20.12 matrices,
// 4.12 vertex components).
//
// No cycle timing: commands are logged and replayed in one pass at VBlank,
// on an observing read, or when the log fills. FIFO always reports empty;
// GXSTAT is synthesised (see read()).

// Parameter count per command; replay advances par_log_ by this.
inline constexpr u8 CMD_PARAMS[256] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0,
  1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
  1, 1, 1, 1, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  // 0x80-0xFF: none
};

struct Vertex {
  s32 pos[4];          // clip space, 20.12
  s32 col[3];          // 5-bit colour, 12 fractional bits (kept through clipping)
  s16 tex[2];          // 12.4 texture coordinates
  bool clipped;
  u8  oc;              // frustum outcode of pos (bits: +x -x +y -y +z -z)
  s32 sx, sy;          // screen position after viewport transform
  s32 fcol[3];         // final 9-bit colour used by the rasteriser
  s32 fx, fy;          // sx, sy in 28.4: the GPU raster's sub-pixel position (sx == fx >> 4), so
                       // a T-junction vertex stays on its neighbour's edge; not hashed, not saved
};

struct Polygon {
  u16 vtx[10];         // indices into the engine's vertex RAM
  u32 nverts;
  s32 z[10], w[10];    // per-vertex depth and normalised W
  bool wbuffer;
  u32 attr, texparam, texpal;
  bool degenerate;
  bool facing;         // front-facing per the culling test
  bool translucent;
  bool shadow_mask, shadow;
  u32 vtop, vbot;      // vtx[] indices of the top and bottom points
  s32 ytop, ybot, xtop, xbot;
  u32 sort_key;
};

template <typename T, u32 N>
class Fifo {
public:
  void clear() { rd_ = wr_ = n_ = 0; }
  bool empty() const { return n_ == 0; }
  bool full() const { return n_ == N; }
  u32 level() const { return n_; }
  void push(const T& v) { buf_[wr_] = v; wr_ = (wr_ + 1) % N; ++n_; }
  T pop() { T v = buf_[rd_]; rd_ = (rd_ + 1) % N; --n_; return v; }
private:
  std::array<T, N> buf_{};
  u32 rd_ = 0, wr_ = 0, n_ = 0;
};

class Gpu3D {
public:
  explicit Gpu3D(NDS& nds);
  // renderer_ must outlive polygon RAM: band workers may still be rasterising the last frame.
  ~Gpu3D();
  void reset();
  template <class S> void sync_state(S& s);   // after sync_raster()

  // Registers: DISP3DCNT (0x60), the 0x320-0x3BF block, the FIFO/command
  // ports and status/results at 0x400-0x6A3.
  static bool owns_reg(u32 addr) {
    const u32 r = addr - 0x04000000;
    return (r >= 0x60 && r < 0x64) || (r >= 0x320 && r < 0x3C0) || (r >= 0x400 && r < 0x6A4);
  }
  u32  read(u32 addr, u32 width);
  void write(u32 addr, u32 width, u32 value);
  // DMA word to GXFIFO (bypasses bus/IO dispatch; same effect as write(0x04000400, 32)).
  void gxfifo_dma_write(u32 value) { if (geometry_on_) gxfifo_write(value); }
  // CPU word store to 0x04000400-0x040005CB: GXFIFO for 0x400-0x43F, direct command port after.
  void gx_port_write(u32 addr, u32 value) {
    const u32 r = addr - 0x04000400;
    if (!geometry_on_) return;
    if (r < 0x40) gxfifo_write(value);
    else log_port(static_cast<u8>((r & 0x1FC) >> 2), value);
  }
  void gxfifo_dma_burst(const u8* src, u32 n);
  // Max words a burst can feed without risk of filling the FIFO.
  u32 fifo_burst_room() const {
    const u32 c = cmd_n_ + 8 < CMD_CAP ? (CMD_CAP - 8 - cmd_n_) >> 2 : 0;
    const u32 q = par_n_ + 8 < PAR_CAP ? PAR_CAP - 8 - par_n_ : 0;
    return c < q ? c : q;
  }

  // POWCNT1 bit 3 (geometry) and bit 2 (rendering).
  void set_powcnt(u16 value);

  bool idle() const { return !geometry_on_ || flush_request_ || cmd_n_ == 0; }
  bool swap_pending() const { return flush_request_ != 0 || swap_wait_; }

  void vblank();            // VCount 192: latch registers, sort, swap buffers
  void render_frame();      // VCount 215: rasterise the latched frame
  // Force next frame to rasterise even if unchanged (frameskip left the picture stale).
  void note_raster_skipped() { render_stale_ = true; }
  Renderer3D::FrameRef frame_ref(bool allow_lag = true) const { return renderer_.frame_ref(allow_lag); }
  u64 last_raster_ns() const { return renderer_.last_band_sum_ns(); }
  const u32* line(const Renderer3D::FrameRef& f, u32 y);
  // The edge plane of that line (Renderer3D::set_edge_export; after line(),
  // so synced), or null when not exported or the 3D layer is scrolled.
  // A GPU frame's edge bytes are made into the plane's words here, with the
  // record's colour (unblended: the GPU raster has no AA blend); a word
  // with bit 31 clear is no edge (the tag below). A pixel in
  // a clump of edge pixels gets none: where a small-polygon mesh (a distant
  // kart, a face, wheel spokes) marks whole areas, the lines are too short to
  // judge and the cuts shred the picture, so a pixel with edge_density_ or
  // more edge pixels in its 3x3 (a straight line has 3, a corner about 5)
  // shows whole (DS_EDGE_DENSITY=N; 0 keeps every pixel).
  // The byte for a pixel with none that an edge run passes through (edge_line's
  // gap fill): where the line crosses a corner of the pixel the band pass
  // found full coverage and wrote nothing, and the tilt either side lost its
  // partner, so the cut steps there. Its two neighbours along the run (the
  // columns either side for a vertical-axis run, the rows for a horizontal
  // one), each holding one byte of that axis within a pixel across, place the
  // line through it; 0 when they don't.
  // The pixels of row `y` edge_gap could fill, in byte lanes: bit 0 a
  // vertical-axis gap (no byte in its column over the three rows, one in
  // each column beside), bit 1 a horizontal one (none in its row three wide,
  // bytes in the rows above and below). A handful per row survive.
  static void edge_gap_candidates(const u8* bytes, u32 y, u8 out[256]) {
    const u8* r = bytes + y * 256;
    const u8* a = y ? r - 256 : nullptr;
    const u8* d = y + 1 < 192 ? r + 256 : nullptr;
    alignas(8) u8 any[272] = {}, ra[272] = {}, rd[272] = {}, rb[272] = {};   // 8 zero either side
    for (u32 x = 0; x < 256; x += 8) {
      const u64 vb = load64(r + x), va = a ? load64(a + x) : 0, vd = d ? load64(d + x) : 0;
      store64(any + 8 + x, vb | va | vd); store64(ra + 8 + x, va); store64(rd + 8 + x, vd); store64(rb + 8 + x, vb);
    }
    const u64 ones = 0x0101010101010101ull;
    for (u32 x = 0; x < 256; x += 8) {
      const u64 cv = (nonzero_lanes(load64(any + 8 + x)) ^ ones) & nonzero_lanes(load64(any + 7 + x)) & nonzero_lanes(load64(any + 9 + x));
      const u64 b3 = load64(rb + 7 + x) | load64(rb + 8 + x) | load64(rb + 9 + x);
      const u64 a3 = load64(ra + 7 + x) | load64(ra + 8 + x) | load64(ra + 9 + x);
      const u64 d3 = load64(rd + 7 + x) | load64(rd + 8 + x) | load64(rd + 9 + x);
      const u64 ch = (nonzero_lanes(b3) ^ ones) & nonzero_lanes(a3) & nonzero_lanes(d3);
      store64(out + x, cv | (ch << 1));
    }
  }
  static u8 edge_gap(const u8* bytes, u32 x, u32 y, u8 kinds) {   // kinds: edge_gap_candidates' bits
    auto pos = [](u8 v) { const float f = static_cast<float>((v & 31) + 1) / 32.0f; return (v & 0x40) ? 1.0f - f : f; };
    // One byte of axis `vert` in the three pixels of `line` (a row or a column) around `at`, its line position across; false if none or more.
    auto one = [&](bool vert, bool column, u32 line, u32 at, float* h) {
      int n = 0;
      for (int k = -1; k <= 1; ++k) {
        const int p = static_cast<int>(at) + k;
        if (p < 0 || p >= (column ? 192 : 256)) continue;
        const u8 v = column ? bytes[p * 256 + line] : bytes[line * 256 + p];
        if (!(v & 0x80) || static_cast<bool>(v & 0x20) != vert) continue;
        if (n++) return false;
        *h = static_cast<float>(k) + pos(v);
      }
      return n == 1;
    };
    auto make = [](bool vert, float h) -> u8 {
      const bool neg = h < 0.5f;
      const int cov = std::clamp(static_cast<int>((neg ? 1.0f - h : h) * 32.0f + 0.5f) - 1, 0, 31);
      return static_cast<u8>(0x80 | (neg ? 0x40 : 0) | (vert ? 0x20 : 0) | cov);
    };
    float hl, hr;
    if ((kinds & 1) && one(true, true, x - 1, y, &hl) && one(true, true, x + 1, y, &hr)) {
      const float hm = (hl + hr) * 0.5f, dh = hr - hl;
      if (hm >= 0.0f && hm <= 1.0f && dh > -1.5f && dh < 1.5f) return make(true, hm);
    }
    if ((kinds & 2) && one(false, false, y - 1, x, &hl) && one(false, false, y + 1, x, &hr)) {
      const float hm = (hl + hr) * 0.5f, dh = hr - hl;
      if (hm >= 0.0f && hm <= 1.0f && dh > -1.5f && dh < 1.5f) return make(false, hm);
    }
    return 0;
  }
  const u32* edge_line(const Renderer3D::FrameRef& f, u32 y) const {
    if (render_xpos_) return nullptr;
    if (!f.edge_bytes) return f.edge_line(y);
    const u8* b = f.edge_bytes + y * 256;
    bool any = false;
    for (u32 x = 0; x < 256 && !any; x += 8) any = load64(b + x) != 0;
    if (!any) { std::memset(edge_words_, 0, sizeof(edge_words_)); return edge_words_; }
    const u32* c = f.line(y);
    // Eight columns at a time in byte lanes: 1 per edge pixel over the three
    // rows (at most 3 a lane), then the three columns (at most 9), then
    // lane + (0x80 - N) has bit 7 set where the count reaches N.
    alignas(8) u8 col[272] = {};   // the 3-row count per column, 8 zero either side
    alignas(8) u8 dense[256];
    if (edge_density_) {
      const u8* a = y ? b - 256 : nullptr;
      const u8* d = y + 1 < 192 ? b + 256 : nullptr;
      for (u32 x = 0; x < 256; x += 8) {
        u64 n = nonzero_lanes(load64(b + x));
        if (a) n += nonzero_lanes(load64(a + x));
        if (d) n += nonzero_lanes(load64(d + x));
        store64(col + 8 + x, n);
      }
      const u64 bias = 0x0101010101010101ull * static_cast<u64>(0x80 - std::min(edge_density_, 0x7F));
      for (u32 x = 0; x < 256; x += 8) {
        const u64 n = load64(col + 7 + x) + load64(col + 8 + x) + load64(col + 9 + x);
        store64(dense + x, (n + bias) & 0x8080808080808080ull);
      }
    } else std::memset(dense, 0, sizeof(dense));
    alignas(8) u8 gap[3][256];   // edge_gap_candidates of rows y - 1, y, y + 1 (the tag loop needs the fills beside)
    if (edge_gap_) for (int k = -1; k <= 1; ++k) { if (static_cast<int>(y) + k >= 0 && static_cast<int>(y) + k < 192) edge_gap_candidates(f.edge_bytes, y + k, gap[k + 1]); }
    else std::memset(gap, 0, sizeof(gap));
    for (u32 x = 0; x < 256; ++x) {
      const u32 w = c[x];
      const u8 e = dense[x] ? 0 : b[x] ? b[x] : gap[1][x] ? edge_gap(f.edge_bytes, x, y, gap[1][x]) : 0;
      edge_words_[x] = e ? (static_cast<u32>(e) << 24) | (w & 63) | (((w >> 8) & 63) << 6) | (((w >> 16) & 63) << 12) | (((w >> 24) & 31) << 18) : 0;
    }
    // The present's blend reaches one pixel past an edge pixel, so a pixel
    // with no edge looks its four neighbours up for one; a tag word (byte
    // 1: bit 7 clear, no edge) on the pixels beside an edge pixel spares
    // the rest that. Beside: along the row by the words, above and below
    // by the bytes (a superset where the density limit dropped one).
    {
      const u8* a = y ? b - 256 : nullptr;
      const u8* d = y + 1 < 192 ? b + 256 : nullptr;
      for (u32 x = 0; x < 256; ++x) {
        if (edge_words_[x]) continue;
        const bool beside = (x && (edge_words_[x - 1] >> 31)) || (x < 255 && (edge_words_[x + 1] >> 31)) || (a && (a[x] || (gap[0][x] && edge_gap(f.edge_bytes, x, y - 1, gap[0][x])))) || (d && (d[x] || (gap[2][x] && edge_gap(f.edge_bytes, x, y + 1, gap[2][x]))));
        if (beside) { const u32 w = c[x]; edge_words_[x] = (1u << 24) | (w & 63) | (((w >> 8) & 63) << 6) | (((w >> 16) & 63) << 12) | (((w >> 24) & 31) << 18); }   // with the pixel's colour and alpha: Engine2D::edge_compose reads them
      }
    }
    return edge_words_;
  }
  static u64 load64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
  static void store64(u8* p, u64 v) { std::memcpy(p, &v, 8); }
  static u64 nonzero_lanes(u64 v) { return ((((v & 0x7F7F7F7F7F7F7F7Full) + 0x7F7F7F7F7F7F7F7Full) | v) >> 7) & 0x0101010101010101ull; }   // 1 in each byte that is not 0
  // Finish async raster; needed before touching texture VRAM (raster workers may still read it).
  void sync_raster();
  void debug_dump(FILE* f) { renderer_.debug_dump(f); }
  void set_render_xpos(u16 value, u16 mask);

  void check_fifo_irq();
  void check_fifo_irq_fast() { if (gxstat_ >> 30) check_fifo_irq(); }
  void check_fifo_dma();

  u32 dispcnt() const { return dispcnt_; }
  const RenderState& render_state() const { return rstate_; }
  const Vertex& vertex(u32 idx) const { return vram_[idx]; }
  const Polygon* const* render_polygons() const { return render_polys_[raster_bank_].data(); }
  u32 render_polygon_count() const { return render_count_[raster_bank_]; }
  // No SWAP_BUFFERS/render-register change since last render: rasteriser may keep prior output.
  bool render_identical() const { return render_identical_; }
  u64 swap_count() const { return swaps_; }

private:
  NDS& nds_;
  Renderer3D renderer_;
public:
  Renderer3D& renderer() { return renderer_; }   // tests
private:

  // cmd_log_: one byte/command. par_log_: its parameters as contiguous u32
  // words in arrival order. Sized for a frame, not a FIFO; replayed at
  // VBlank, on an observing read, or here if either log nears full.
  // par_log_ is over-allocated by PAR_SLACK so exec_single can read p[0] for
  // a zero-parameter command at the very end of the log.
  static constexpr u32 CMD_CAP = 1u << 16, PAR_CAP = 1u << 18, PAR_SLACK = 64;
  std::unique_ptr<u8[]> cmd_log_ = std::unique_ptr<u8[]>(new u8[CMD_CAP]());
  std::unique_ptr<u32[]> par_log_ = std::unique_ptr<u32[]>(new u32[PAR_CAP + PAR_SLACK]());
  u32 cmd_n_ = 0, par_n_ = 0;
  bool swapped_ = false;         // a SWAP_BUFFERS finalised a list since the last VBlank
  // SWAP_BUFFERS issued since last VBlank; already executed, but GXSTAT bit 27
  // reads busy until swap_busy_until_ (ARM9 cycles), modelling the ~325-cycle
  // post-flip busy window, set at VBlank.
  bool swap_wait_ = false;
  u64 swap_busy_until_ = 0;
  bool list_same_ = false;       // finalise_list: the finished list equals the previous one

  // A command under assembly writes parameters at par_log_[par_n_ ...] and
  // commits by appending its byte and advancing par_n_. Packed GXFIFO and
  // direct command ports both assemble here so they can't interleave into a
  // corrupt log; a port write mid-FIFO-command abandons that command.
  u32 inflight_n_ = 0;
  u8  inflight_cmd_ = 0xFF;
  struct Sink;                    // GXFIFO sink; defined in the .cpp
  friend struct Sink;
  void log_commit(u8 cmd) { cmd_log_[cmd_n_++] = cmd; par_n_ += inflight_n_; inflight_n_ = 0; inflight_cmd_ = 0xFF; }
  void log_room() { if (cmd_n_ + 8 >= CMD_CAP || par_n_ + 40 >= PAR_CAP) drain_all(); }
  // Direct command port: one parameter of `cmd` per write.
  void log_port(u8 cmd, u32 value) {
    log_room();
    const u32 np = CMD_PARAMS[cmd];
    if (np == 0) { cmd_log_[cmd_n_++] = cmd; return; }
    if (cmd != inflight_cmd_) { inflight_cmd_ = cmd; inflight_n_ = 0; }
    par_log_[par_n_ + inflight_n_] = value;
    if (++inflight_n_ >= np) log_commit(cmd);
  }

  // Packed-command parser state; a struct so a DMA burst can walk with a
  // local copy and write it back once, while the single-word port uses the member directly.
  struct GxParse { u32 num_cmds = 0, cur_cmd = 0, param_count = 0, total_params = 0; };
  GxParse parse_;

  // gxstat_ carries only the IRQ-mode bits (30-31); busy/FIFO-level/stack-test
  // bits are synthesised elsewhere since an observing read replays the log first.
  u32 gxstat_ = 0;
  u32 box_result_ = 0;           // GXSTAT bit 1 (box test result)
  u32 stack_err_ = 0;            // GXSTAT bit 15 (matrix stack over/underflow); read() ORs it in

  void stack_reset();                   // GXSTAT bit 15 written: clear flag, reset proj/tex stacks
  bool geometry_on_ = false, rendering_on_ = false;
  u32 dispcnt_ = 0;
  u8  alpha_ref_val_ = 0, alpha_ref_ = 0;
  std::array<u16, 32> toon_{};
  std::array<u16, 8> edge_{};
  u32 fog_color_ = 0, fog_offset_ = 0;
  std::array<u8, 32> fog_density_{};
  u32 clear_attr1_ = 0x3F000000, clear_attr2_ = 0x00007FFF;
  u32 zero_dot_w_limit_ = 0xFFFFFF;
  RenderState rstate_;
  // BG0HOFS of engine A, applied from its journal in display-line order on the compositor thread.
  u16 render_xpos_ = 0;
  mutable u32 edge_words_[256] = {};   // edge_line's GPU-frame row
  int edge_density_ = 6;                // edge_line: the 3x3 count that drops a pixel (set at construction from DS_EDGE_DENSITY)
  bool edge_gap_ = true;                // edge_line: fill the pixels a run passes through without a byte (DS_EDGE_GAP=0 off)
  std::atomic<bool> render_on_{false};
  alignas(16) u32 scrolled_[256] = {};

  // Matrices (20.12, row-major: m[row*4+col]).
  u32 matrix_mode_ = 0;
  std::array<s32, 16> proj_, pos_, vec_, tex_, clip_;
  bool clip_dirty_ = true;
  std::array<s32, 16> proj_stack_, tex_stack_;
  std::array<std::array<s32, 16>, 32> pos_stack_, vec_stack_;
  s32 proj_sp_ = 0, pos_sp_ = 0, tex_sp_ = 0;
  std::array<u32, 6> viewport_{};

  // Vertex state.
  u32 poly_mode_ = 0;
  s16 cur_vertex_[3] = {};
  u8  vertex_color_[3] = {};
  s16 texcoords_[2] = {}, raw_texcoords_[2] = {};
  s16 normal_[3] = {};
  s16 light_dir_[4][3] = {};
  s32 spec_recip_[4] = {};
  u8  light_color_[4][3] = {};
  u8  mat_diffuse_[3] = {}, mat_ambient_[3] = {}, mat_specular_[3] = {}, mat_emission_[3] = {};
  bool use_shininess_ = false;
  std::array<u8, 128> shininess_{};
  u32 polygon_attr_ = 0, cur_polygon_attr_ = 0;
  u32 texparam_ = 0, texpal_ = 0;
  s32 pos_test_[4] = {};
  s16 vec_test_[3] = {};

  // Strip assembly carries two vertices into the next polygon; vptr_ maps a
  // position in the polygon being assembled to the slot holding it (a
  // permutation of 0..3) instead of copying structs.
  Vertex temp_vtx_[4] = {};
  Vertex* vptr_[4] = {&temp_vtx_[0], &temp_vtx_[1], &temp_vtx_[2], &temp_vtx_[3]};
  void reset_vptr() { for (int i = 0; i < 4; ++i) vptr_[i] = &temp_vtx_[i]; }
  u32 vertex_num_ = 0, vertex_in_poly_ = 0, consecutive_polys_ = 0;
  Polygon* last_strip_poly_ = nullptr;
  u32 num_opaque_ = 0;

  // Four banks: bank_ is being written, render_bank_ holds the finalised
  // list for the next render(), raster_bank_ is what an in-flight raster
  // reads, pending_bank_ is what VBlank chose for the next render_frame. A
  // swap retires bank_ into render_bank_ and picks the bank named by none
  // of the others.
  static constexpr u32 VRAM_BANK = 6144, PRAM_BANK = 2048, BANKS = 4;
  std::vector<Vertex> vram_ = std::vector<Vertex>(VRAM_BANK * BANKS);
  std::vector<Polygon> pram_ = std::vector<Polygon>(PRAM_BANK * BANKS);
  u32 bank_ = 0, render_bank_ = 1, raster_bank_ = 1;
  u32 pending_bank_ = 1;
  std::mutex bank_mu_;   // guards bank role changes across threads
  u32 next_write_bank() const { for (u32 b = 0; b < BANKS; ++b) if (b != render_bank_ && b != raster_bank_ && b != pending_bank_) return b; return 0; }
  u32 num_vertices_ = 0, num_polygons_ = 0;
  // Sorted list per bank: a SWAP may finalise the next list while the previous one's raster runs.
  std::array<std::array<const Polygon*, PRAM_BANK>, BANKS> render_polys_{};
  std::array<u32, BANKS> render_count_{};
  u64 swaps_ = 0;
  bool list_unconsumed_ = false;   // a finalised list is waiting for a render (C_GX_LIST_DROPPED)
  bool render_identical_ = false;
  bool render_stale_ = false;      // last render is older than rstate_ says (note_raster_skipped)
  u32 flush_request_ = 0, flush_attr_ = 0;
  u64 census_prev_hash_ = 0;          // DS_CENSUS_GX: hash of the last submitted list
  bool census_have_prev_ = false;
  u32 census_prev_polys_ = 0, census_prev_verts_ = 0;
  bool census_have_prev_counts_ = false;
  u32 prev_swap_polys_ = 0, prev_swap_verts_ = 0;   // other bank's list size
  bool rendered_before_ = false;

  Vertex* cur_vram() { return &vram_[bank_ * VRAM_BANK]; }
  Polygon* cur_pram() { return &pram_[bank_ * PRAM_BANK]; }
  u32 vram_base() const { return bank_ * VRAM_BANK; }

  // Shared by the single-word port and the burst (same state machine, differing only in sink).
  template <class S> [[gnu::always_inline]] static inline void gxfifo_word(u32 value, GxParse& p, S& sink);
  void drain_all();               // replay everything queued, now
  void finalise_list();           // SWAP command: sort polygon list, detect repeat of previous
  void normalise_temp_vtx();      // put temp_vtx_ back in position order, reset vptr_ to identity
  void gxfifo_write(u32 value);
  void exec_single(u8 cmd, const u32* p);


  // Geometry.
  void update_clip_matrix();
  void submit_vertex();
  void submit_polygon();
  // Out of line so the common reject path carries neither's stack frame/register pressure.
  __attribute__((noinline)) void emit_polygon_unclipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, bool facing);
  __attribute__((noinline)) void emit_polygon_clipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, int lastpolyverts, bool facing);
  Polygon* new_polygon(bool facing);
  static u8 outcode(const s32* pos);
  void finish_polygon(Polygon* poly, int nverts);
  void calculate_lighting();
  void box_test(const u32* params);
  void pos_test();
  void vec_test(u32 param);
  void reset_render_state();
};

} // namespace ds::gpu
