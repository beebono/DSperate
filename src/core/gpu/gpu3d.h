// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/render3d.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

// 3D geometry engine: command FIFO, matrix stacks, lighting, clipping,
// viewport transform and the double-buffered vertex/polygon RAM the
// rasteriser consumes. Fixed-point integer math throughout (20.12 matrices,
// 4.12 vertex components), with the hardware's truncation points as
// documented by the melonDS project (GPLv3), whose software implementation
// this engine is verified against frame for frame.
//
// Timing: the engine has its own clock (system cycles, half the ARM9 clock).
// The scheduler calls run_to() after every ARM9 slice and register reads
// catch up first, so command execution is interleaved with the CPU at slice
// granularity. When the 256-entry FIFO is full the ARM9 (and its DMA) stall
// until it drains.

struct Vertex {
  s32 pos[4];          // clip space, 20.12
  s32 col[3];          // 5-bit colour with 12 fractional bits (kept through clipping)
  s16 tex[2];          // 12.4 texture coordinates
  bool clipped;
  s32 sx, sy;          // screen position after the viewport transform
  s32 fcol[3];         // final 9-bit colour used by the rasteriser
};

struct Polygon {
  u16 vtx[10];         // indices into the engine's vertex RAM
  u32 nverts;
  s32 z[10], w[10];    // per-vertex depth and normalised W
  bool wbuffer;
  u32 attr, texparam, texpal;
  bool degenerate;
  bool facing;         // front-facing as seen by the culling test
  bool translucent;
  bool shadow_mask, shadow;
  u32 vtop, vbot;      // vertex indices (into vtx[]) of the top and bottom points
  s32 ytop, ybot, xtop, xbot;
  u32 sort_key;
};

// Registers latched at VBlank for the rasteriser.
struct RenderState {
  u32 dispcnt = 0;
  u8  alpha_ref = 0;
  std::array<u16, 32> toon{};
  std::array<u16, 8> edge{};
  u32 fog_color = 0, fog_offset = 0, fog_shift = 0;
  std::array<u8, 34> fog_density{};
  u32 clear_attr1 = 0x3F000000, clear_attr2 = 0x00007FFF;
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
  // A DMA word landing on GXFIFO, without the bus and I/O dispatch a
  // register write goes through (the same semantics as write(0x04000400, 32)).
  void gxfifo_dma_write(u32 value) { if (geometry_on_) gxfifo_write(value); }
  // A run of `n` DMA words fed straight from a direct-mapped source page.
  // See the definition for why the burst is unobservable and what that buys.
  void gxfifo_dma_burst(const u8* src, u32 n);
  // Words a burst may feed without any chance of filling the FIFO: a word
  // carries at most four commands, so this many can never reach FIFO_DEPTH.
  u32 fifo_burst_room() const { return (FIFO_DEPTH - fifo_n_) >> 2; }

  // POWCNT1 bit 3 (geometry) and bit 2 (rendering).
  void set_powcnt(u16 value);

  // Advance the engine to `arm9_time` (scheduler time, ARM9 cycles). The
  // idle check is inline: the scheduler calls this after every ARM9 slice.
  void run_to(u64 arm9_time) {
    if (!geometry_on_ || flush_request_ || (pipe_n_ == 0 && !(gxstat_ & (1u << 27)))) { timestamp_ = arm9_time >> 1; return; }
    run_to_slow(arm9_time);
  }
  bool stalled() const { return stalled_; }
  // Nothing to execute and nothing to raise: run_to would only stamp the time.
  bool idle() const { return !geometry_on_ || flush_request_ || (pipe_n_ == 0 && !(gxstat_ & (1u << 27))); }
  // A swap has been issued and waits for VBlank: the engine accepts nothing
  // and changes nothing until then, so a loop polling GXSTAT can be skipped.
  bool swap_pending() const { return flush_request_ != 0; }

  // Display timing hooks.
  void vblank();            // VCount 192: latch registers, sort, swap buffers
  void render_frame();      // VCount 215: rasterise the latched frame
  const u32* line(u32 y);   // 3D output for display line y, X-scrolled (RGB666 + 5-bit alpha at 24-28)
  // Force the asynchronous raster to finish. Called wherever something is
  // about to change what its workers are reading -- in practice only
  // Bus::update_vram, since texture VRAM is unreachable any other way.
  void sync_raster();
  void debug_dump(FILE* f) { renderer_.debug_dump(f); }
  void set_render_xpos(u16 value, u16 mask);

  // The FIFO IRQ line follows the FIFO level in the two IRQ modes; with the
  // mode off the line is already down (the GXSTAT write handlers call the
  // full check), so the per-command path tests the mode first.
  void check_fifo_irq();
  void check_fifo_irq_fast() { if (gxstat_ >> 30) check_fifo_irq(); }
  void check_fifo_dma();

  u32 dispcnt() const { return dispcnt_; }
  const RenderState& render_state() const { return rstate_; }
  const Vertex& vertex(u32 idx) const { return vram_[idx]; }
  const Polygon* const* render_polygons() const { return render_polys_.data(); }
  u32 render_polygon_count() const { return render_count_; }
  // No SWAP_BUFFERS since the last render and the render registers are
  // unchanged: the rasteriser may keep its previous output if the textures
  // it used are unchanged too (it checks those itself).
  bool render_identical() const { return render_identical_; }

private:
  NDS& nds_;
  Renderer3D renderer_;
public:
  Renderer3D& renderer() { return renderer_; }   // tests
private:

  struct Entry { u32 param; u8 cmd; };
  // The command pipe (4), the FIFO (256) and the CPU's stalled writes (64)
  // are one ring in arrival order: the pipe is its head, the FIFO the middle,
  // the stall queue the tail. Only the three counts move on a push or pop;
  // no entry is ever copied between stages. Visible state is unchanged:
  // GXSTAT reports fifo_n_, the pipe refills to 3+ from the FIFO on a pop.
  static constexpr u32 PIPE_DEPTH = 4, FIFO_DEPTH = 256, STALL_DEPTH = 64, RING = 512;
  std::array<Entry, RING> ring_{};
  u32 ring_rd_ = 0, ring_wr_ = 0;
  u32 pipe_n_ = 0, fifo_n_ = 0, stall_n_ = 0;
  bool stalled_ = false;
  bool pipe_empty() const { return pipe_n_ == 0; }
  u32  fifo_level() const { return fifo_n_; }
  void ring_push(const Entry& e) { ring_[ring_wr_] = e; ring_wr_ = (ring_wr_ + 1) & (RING - 1); }
  // Status side effects of an entry entering the pipe or FIFO (not the stall queue).
  void note_enqueued(u8 cmd) {
    gxstat_ |= (1u << 27);
    if (static_cast<u8>(cmd - 0x11) <= 1) { gxstat_ |= (1u << 14); ++num_pushpop_; }      // 0x11, 0x12
    else if (static_cast<u8>(cmd - 0x70) <= 2) { gxstat_ |= (1u << 0); ++num_tests_; }    // 0x70-0x72
  }

  // Command assembly for packed GXFIFO writes.
  u32 num_cmds_ = 0, cur_cmd_ = 0, param_count_ = 0, total_params_ = 0;
  std::array<u32, 32> exec_params_{};
  u32 exec_count_ = 0;

  // Timing.
  u64 timestamp_ = 0;          // system cycles
  s32 cycle_count_ = 0;
  s32 vertex_pipeline_ = 0, normal_pipeline_ = 0, polygon_pipeline_ = 0;
  s32 vertex_slot_counter_ = 0;
  u32 vertex_slots_free_ = 1;
  u32 num_pushpop_ = 0, num_tests_ = 0;

  // Status.
  u32 gxstat_ = 0;
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
  u16 render_xpos_ = 0;

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

  // Polygon assembly.
  Vertex temp_vtx_[4] = {};
  u32 vertex_num_ = 0, vertex_in_poly_ = 0, consecutive_polys_ = 0;
  Polygon* last_strip_poly_ = nullptr;
  u32 num_opaque_ = 0;

  // Vertex/polygon RAM, two banks each.
  static constexpr u32 VRAM_BANK = 6144, PRAM_BANK = 2048;
  std::array<Vertex, VRAM_BANK * 2> vram_{};
  std::array<Polygon, PRAM_BANK * 2> pram_{};
  u32 bank_ = 0;
  u32 num_vertices_ = 0, num_polygons_ = 0;
  std::array<const Polygon*, PRAM_BANK> render_polys_{};
  u32 render_count_ = 0;
  bool render_identical_ = false;
  u32 flush_request_ = 0, flush_attr_ = 0;
  u64 census_prev_hash_ = 0;          // DS_CENSUS_GX: hash of the last submitted list
  bool census_have_prev_ = false;
  u32 census_prev_polys_ = 0, census_prev_verts_ = 0;
  bool census_have_prev_counts_ = false;
  u32 prev_swap_polys_ = 0, prev_swap_verts_ = 0;   // DS_R3D_SKIPDUP: the other bank's list size
  bool rendered_before_ = false;

  Vertex* cur_vram() { return &vram_[bank_ * VRAM_BANK]; }
  Polygon* cur_pram() { return &pram_[bank_ * PRAM_BANK]; }
  u32 vram_base() const { return bank_ * VRAM_BANK; }

  // FIFO.
  [[gnu::always_inline]] void fifo_write(const Entry& e);   // LTO outlined it out of gxfifo_write: 30 insn + a call per word
  // The packed-command walk, shared by the single-word port and the burst.
  // The two differ only in their sink, so the assembly state machine has one
  // copy: a divergence between them would be a silent accuracy bug.
  template <class Push> [[gnu::always_inline]] void gxfifo_word(u32 value, Push push);
  Entry fifo_read();
  void gxfifo_write(u32 value);
  void run_to_slow(u64 arm9_time);
  __attribute__((always_inline)) void execute();       // one call site: the run_to_slow loop
  __attribute__((always_inline)) void exec_single(u8 cmd, u32 param);   // one call site: execute()
  void exec_multi(u8 cmd);

  // Per-command timing helpers: called once per command from the execute
  // loop, so they are forced inline (LTO left them as calls: ~15 insn of
  // call overhead each at 15 k+ calls a frame).
  __attribute__((always_inline)) void add_cycles(s32 n);
  void next_vertex_slot();
  void stall_polygon_pipeline(s32 delay, s32 nonstall_delay);
  __attribute__((always_inline)) void vtx_cmd_submit();
  __attribute__((always_inline)) void vtx_cmd_delayed6();
  __attribute__((always_inline)) void vtx_cmd_delayed8();
  __attribute__((always_inline)) void vtx_cmd_delayed4();
  void finish_work(s32 cycles);

  // Geometry.
  void update_clip_matrix();
  void submit_vertex();
  void submit_polygon();
  void calculate_lighting();
  void box_test(const u32* params);
  void pos_test();
  void vec_test(u32 param);
  void reset_render_state();
};

} // namespace ds::gpu
