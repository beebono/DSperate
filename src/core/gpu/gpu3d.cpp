// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu3d.h"
#include "core/nds.h"
#include "core/profile.h"
#if DSPERATE_NEON
#include <arm_neon.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

namespace {

// Parameter count per command. Commands with zero parameters still occupy
// one FIFO entry when written through the packed GXFIFO port.
constexpr u8 CMD_PARAMS[256] = {
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

// ---- census (DS_CENSUS_GX=1) -----------------------------------------------
// Is the polygon list a game submits on a SWAP_BUFFERS actually different from
// the one we last rasterised? `render_identical_` below only ever answers "no
// swap happened at all", so a game that re-runs its render loop and resubmits
// byte-identical geometry is fully re-rasterised. This hash sizes what a
// content comparison would recover. It walks every polygon and every vertex,
// so it is off unless asked for.
//
// The vertex indices in Polygon::vtx are absolute into the double-buffered
// vertex RAM and therefore alternate with the bank -- hash the vertex records
// they point at, never the indices themselves.
bool census_gx() { static const bool on = std::getenv("DS_CENSUS_GX") != nullptr; return on; }

inline void fnv(u64& h, u64 v) { h = (h ^ v) * 0x100000001B3ull; }

u64 census_list_hash(const Polygon* const* polys, u32 n, const Vertex* vram) {
  u64 h = 0xCBF29CE484222325ull;
  fnv(h, n);
  for (u32 i = 0; i < n; ++i) {
    const Polygon& p = *polys[i];
    fnv(h, p.nverts); fnv(h, p.attr); fnv(h, p.texparam); fnv(h, p.texpal);
    fnv(h, p.wbuffer); fnv(h, p.degenerate); fnv(h, p.facing); fnv(h, p.translucent);
    fnv(h, p.shadow_mask); fnv(h, p.shadow);
    fnv(h, p.vtop); fnv(h, p.vbot);
    fnv(h, static_cast<u32>(p.ytop)); fnv(h, static_cast<u32>(p.ybot));
    fnv(h, static_cast<u32>(p.xtop)); fnv(h, static_cast<u32>(p.xbot));
    fnv(h, p.sort_key);
    for (u32 v = 0; v < p.nverts && v < 10; ++v) {
      fnv(h, static_cast<u32>(p.z[v])); fnv(h, static_cast<u32>(p.w[v]));
      const Vertex& vt = vram[p.vtx[v]];
      for (int c = 0; c < 4; ++c) fnv(h, static_cast<u32>(vt.pos[c]));
      for (int c = 0; c < 3; ++c) fnv(h, static_cast<u32>(vt.col[c]));
      for (int c = 0; c < 3; ++c) fnv(h, static_cast<u32>(vt.fcol[c]));
      fnv(h, static_cast<u16>(vt.tex[0])); fnv(h, static_cast<u16>(vt.tex[1]));
      fnv(h, vt.clipped);
      fnv(h, static_cast<u32>(vt.sx)); fnv(h, static_cast<u32>(vt.sy));
    }
  }
  return h;
}

// Would a straight comparison against the previous frame beat hashing? The
// polygon and vertex RAM are already double-buffered, so last frame's list is
// still resident in the other bank -- a compare needs no copy and no saved
// state, and unlike a hash it can stop at the first difference. This models
// that: bytes scanned before the first mismatch against bytes if scanned in
// full. Measurement only; it does the naive byte walk on purpose.
//
// Polygon::vtx (bytes 0-19) holds ABSOLUTE vertex indices, so the two banks
// differ there by vram_base() on every polygon -- compare it bias-corrected
// and the rest of the struct flat.
// DS_R3D_SKIPDUP=1: keep the previous rendered frame when a SWAP_BUFFERS
// submits the same geometry as the last one. The polygon and vertex RAM are
// double-buffered, so the previous list is still in the other bank -- this is
// an exact comparison, not a hash, so there is no collision risk, and it stops
// at the first difference (measured: 1-20% of the data on four of five scenes).
//
// It must walk fields rather than memcmp the arrays: vtx/z/w are fixed
// 10-element slots of which only `nverts` are written, so the tails hold stale
// data, and vtx holds bank-biased absolute indices.
// On by default since 2026-08-28: RG DS knob sweep, paired 3 reps, -0.7 % (mlbis)
// to -2.0 % (meteos) on the five replay scenes, flat on GSDD, 1/17 reps slower.
// DS_R3D_SKIPDUP=0 turns it off for A/B.
bool skip_dup() { static const bool on = [] { const char* e = std::getenv("DS_R3D_SKIPDUP"); return !e || std::atoi(e) != 0; }(); return on; }

bool lists_equal(const Polygon* a, const Polygon* b, u32 npoly, u32 abase, u32 bbase, const Vertex* vram) {
  for (u32 i = 0; i < npoly; ++i) {
    const Polygon& p = a[i]; const Polygon& q = b[i];
    if (p.nverts != q.nverts) return false;
    if (std::memcmp(&p.attr, &q.attr, 12) != 0) return false;          // attr, texparam, texpal
    if (p.wbuffer != q.wbuffer) return false;
    if (std::memcmp(&p.degenerate, &q.degenerate, 5) != 0) return false;
    if (std::memcmp(&p.vtop, &q.vtop, 28) != 0) return false;          // vtop..sort_key
    const u32 nv = p.nverts <= 10 ? p.nverts : 10;
    if (std::memcmp(p.z, q.z, nv * 4) != 0) return false;
    if (std::memcmp(p.w, q.w, nv * 4) != 0) return false;
    for (u32 v = 0; v < nv; ++v) {
      if (p.vtx[v] - abase != q.vtx[v] - bbase) return false;
      const Vertex& s0 = vram[p.vtx[v]]; const Vertex& t0 = vram[q.vtx[v]];
      if (std::memcmp(s0.pos, t0.pos, 16) != 0) return false;
      if (std::memcmp(s0.col, t0.col, 12) != 0) return false;
      if (std::memcmp(s0.tex, t0.tex, 4) != 0) return false;
      if (s0.clipped != t0.clipped) return false;
      if (std::memcmp(&s0.sx, &t0.sx, 8) != 0) return false;
      if (std::memcmp(s0.fcol, t0.fcol, 12) != 0) return false;
    }
  }
  return true;
}

struct CmpModel { u64 early, full; };

// NOTE: the polygon array is NOT flat-comparable. `vtx`, `z` and `w` are
// fixed 10-element slots of which only `nverts` are ever written (see
// submit_polygon), so the tails hold stale data from whatever polygon last
// occupied the slot. A straight memcmp over pram_ mismatches on that garbage
// immediately. Compare the live fields only -- the same set the hash covers,
// in the same order.
CmpModel census_compare(const Polygon* a, const Polygon* b, u32 npoly, u32 abase, u32 bbase,
                        const Vertex* va, const Vertex* vb) {
  CmpModel m{0, 0};
  bool done = false;
  auto eq = [&](const void* x, const void* y, u32 n) {
    m.full += n;
    if (done) return;
    m.early += n;
    if (std::memcmp(x, y, n) != 0) done = true;
  };
  for (u32 i = 0; i < npoly; ++i) {
    const Polygon& p = a[i]; const Polygon& q = b[i];
    eq(&p.nverts, &q.nverts, 4);
    eq(&p.attr, &q.attr, 12);                       // attr, texparam, texpal
    eq(&p.wbuffer, &q.wbuffer, 1);
    eq(&p.degenerate, &q.degenerate, 5);            // the five flag bytes
    eq(&p.vtop, &q.vtop, 28);                       // vtop..sort_key, contiguous
    const u32 nv = p.nverts == q.nverts && p.nverts <= 10 ? p.nverts : 0;
    eq(p.z, q.z, nv * 4);
    eq(p.w, q.w, nv * 4);
    for (u32 v = 0; v < nv; ++v) {
      m.full += 2;
      if (!done) { m.early += 2; if (p.vtx[v] - abase != q.vtx[v] - bbase) done = true; }
      const Vertex& s0 = va[p.vtx[v]]; const Vertex& t0 = vb[q.vtx[v]];
      eq(s0.pos, t0.pos, 16);
      eq(s0.col, t0.col, 12);
      eq(s0.tex, t0.tex, 4);
      eq(&s0.clipped, &t0.clipped, 1);
      eq(&s0.sx, &t0.sx, 8);                        // sx, sy contiguous
      eq(s0.fcol, t0.fcol, 12);
    }
  }
  return m;
}

inline void mtx_identity(s32* m) {
  for (int i = 0; i < 16; ++i) m[i] = 0;
  m[0] = m[5] = m[10] = m[15] = 0x1000;
}
inline void mtx_load_4x3(s32* m, const s32* s) {
  m[0] = s[0]; m[1] = s[1]; m[2] = s[2]; m[3] = 0;
  m[4] = s[3]; m[5] = s[4]; m[6] = s[5]; m[7] = 0;
  m[8] = s[6]; m[9] = s[7]; m[10] = s[8]; m[11] = 0;
  m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}
// m = s * m, with s a 4x4 / 4x3 (implicit last column 0,0,0,1) / 3x3 matrix.
inline void mtx_mult_4x4(s32* m, const s32* s) {
#if DSPERATE_NEON
  // Same 64-bit products, sums and arithmetic shift as the scalar form, two
  // columns per vector: the clip matrix is rebuilt on the first vertex after
  // every position-matrix change (3.5 k times a frame on Golden Sun's title,
  // 43 % of submit_vertex), and sixteen outputs share sixteen inputs.
  const int32x2_t t0l = vld1_s32(m), t0h = vld1_s32(m + 2), t1l = vld1_s32(m + 4), t1h = vld1_s32(m + 6);
  const int32x2_t t2l = vld1_s32(m + 8), t2h = vld1_s32(m + 10), t3l = vld1_s32(m + 12), t3h = vld1_s32(m + 14);
  for (int r = 0; r < 4; ++r) {
    const int32x4_t sr = vld1q_s32(s + r * 4);
    int64x2_t lo = vmull_lane_s32(t0l, vget_low_s32(sr), 0), hi = vmull_lane_s32(t0h, vget_low_s32(sr), 0);
    lo = vmlal_lane_s32(lo, t1l, vget_low_s32(sr), 1);  hi = vmlal_lane_s32(hi, t1h, vget_low_s32(sr), 1);
    lo = vmlal_lane_s32(lo, t2l, vget_high_s32(sr), 0); hi = vmlal_lane_s32(hi, t2h, vget_high_s32(sr), 0);
    lo = vmlal_lane_s32(lo, t3l, vget_high_s32(sr), 1); hi = vmlal_lane_s32(hi, t3h, vget_high_s32(sr), 1);
    vst1_s32(m + r * 4, vmovn_s64(vshrq_n_s64(lo, 12)));
    vst1_s32(m + r * 4 + 2, vmovn_s64(vshrq_n_s64(hi, 12)));
  }
#else
  s32 t[16]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 4]) * t[c] + static_cast<s64>(s[r * 4 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 4 + 2]) * t[8 + c] + static_cast<s64>(s[r * 4 + 3]) * t[12 + c]) >> 12);
#endif
}
inline void mtx_mult_4x3(s32* m, const s32* s) {
  s32 t[16]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 3]) * t[c] + static_cast<s64>(s[r * 3 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 3 + 2]) * t[8 + c]) >> 12);
  for (int c = 0; c < 4; ++c)
    m[12 + c] = static_cast<s32>((static_cast<s64>(s[9]) * t[c] + static_cast<s64>(s[10]) * t[4 + c] +
                                  static_cast<s64>(s[11]) * t[8 + c] + static_cast<s64>(0x1000) * t[12 + c]) >> 12);
}
inline void mtx_mult_3x3(s32* m, const s32* s) {
  s32 t[12]; std::memcpy(t, m, sizeof t);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r * 3]) * t[c] + static_cast<s64>(s[r * 3 + 1]) * t[4 + c] +
                                       static_cast<s64>(s[r * 3 + 2]) * t[8 + c]) >> 12);
}
inline void mtx_scale(s32* m, const s32* s) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = static_cast<s32>((static_cast<s64>(s[r]) * m[r * 4 + c]) >> 12);
}
inline void mtx_translate(s32* m, const s32* s) {
  for (int c = 0; c < 4; ++c)
    m[12 + c] += static_cast<s32>((static_cast<s64>(s[0]) * m[c] + static_cast<s64>(s[1]) * m[4 + c] + static_cast<s64>(s[2]) * m[8 + c]) >> 12);
}

inline s16 sext10(u32 v) { return static_cast<s16>(static_cast<s16>(v << 6) >> 6); }

// ---- clipping -----------------------------------------------------------------
// Sutherland-Hodgman against the six clip planes in the order Z, Y, X. A
// clipped vertex's attributes are interpolated in 64-bit with truncation.

template <int comp, int plane, bool attribs>
void clip_segment(Vertex& out, const Vertex& vin, const Vertex& vout) {
  const s64 num = vin.pos[3] - static_cast<s64>(plane) * vin.pos[comp];
  const s32 den = static_cast<s32>(num - (vout.pos[3] - static_cast<s64>(plane) * vout.pos[comp]));
  auto lerp = [&](s32 a, s32 b) -> s32 { return static_cast<s32>(a + ((static_cast<s64>(b) - a) * num) / den); };
  if (comp != 0) out.pos[0] = lerp(vin.pos[0], vout.pos[0]);
  if (comp != 1) out.pos[1] = lerp(vin.pos[1], vout.pos[1]);
  if (comp != 2) out.pos[2] = lerp(vin.pos[2], vout.pos[2]);
  out.pos[3] = lerp(vin.pos[3], vout.pos[3]);
  out.pos[comp] = plane * out.pos[3];
  if (attribs) {
    out.col[0] = lerp(vin.col[0], vout.col[0]);
    out.col[1] = lerp(vin.col[1], vout.col[1]);
    out.col[2] = lerp(vin.col[2], vout.col[2]);
    out.tex[0] = static_cast<s16>(lerp(vin.tex[0], vout.tex[0]));
    out.tex[1] = static_cast<s16>(lerp(vin.tex[1], vout.tex[1]));
  }
  out.clipped = true;
}

template <int comp, bool attribs>
int clip_against_plane(Vertex* v, int nverts, int clipstart, bool far_clip) {
  Vertex temp[10];
  int c = clipstart;
  if (clipstart == 2) { temp[0] = v[0]; temp[1] = v[1]; }
  // The passes read one array and write the other, so the working vertex is
  // a reference: the value copies were 60 bytes per vertex per pass, and
  // Golden Sun's screen-sized quads clip on every frame.
  for (int i = clipstart; i < nverts; ++i) {
    const int prev = i == 0 ? nverts - 1 : i - 1;
    const int next = i + 1 >= nverts ? 0 : i + 1;
    const Vertex& vtx = v[i];
    if (vtx.pos[comp] > vtx.pos[3]) {
      if (comp == 2 && !far_clip) return 0;       // polygons crossing the far plane are dropped unless bit 12 allows them
      if (v[prev].pos[comp] <= v[prev].pos[3]) clip_segment<comp, 1, attribs>(temp[c++], vtx, v[prev]);
      if (v[next].pos[comp] <= v[next].pos[3]) clip_segment<comp, 1, attribs>(temp[c++], vtx, v[next]);
    } else temp[c++] = vtx;
  }
  nverts = c; c = clipstart;
  for (int i = clipstart; i < nverts; ++i) {
    const int prev = i == 0 ? nverts - 1 : i - 1;
    const int next = i + 1 >= nverts ? 0 : i + 1;
    const Vertex& vtx = temp[i];
    if (vtx.pos[comp] < -vtx.pos[3]) {
      if (temp[prev].pos[comp] >= -temp[prev].pos[3]) clip_segment<comp, -1, attribs>(v[c++], vtx, temp[prev]);
      if (temp[next].pos[comp] >= -temp[next].pos[3]) clip_segment<comp, -1, attribs>(v[c++], vtx, temp[next]);
    } else v[c++] = vtx;
  }
  // Colours keep only their 5-bit integer part across a clip stage.
  for (int i = 0; i < c; ++i)
    for (int k = 0; k < 3; ++k) { v[i].col[k] &= ~0xFFF; v[i].col[k] += 0xFFF; }
  return c;
}

template <bool attribs>
int clip_polygon(Vertex* v, int nverts, int clipstart, bool far_clip) {
  // Most polygons need no clipping at all; the three plane passes would
  // still copy every vertex twice each. Test once and keep only the colour
  // truncation the passes apply (it is idempotent, so once is the same as
  // three times). Vertices before clipstart are reused unclipped ones and
  // are not tested by the passes either.
  bool inside = true;
  for (int i = clipstart; i < nverts; ++i) {
    const Vertex& t = v[i];
    const s32 w = t.pos[3];
    if (t.pos[0] > w || t.pos[0] < -w || t.pos[1] > w || t.pos[1] < -w || t.pos[2] > w || t.pos[2] < -w) { inside = false; break; }
  }
  if (inside) {
    for (int i = 0; i < nverts; ++i)
      for (int k = 0; k < 3; ++k) { v[i].col[k] &= ~0xFFF; v[i].col[k] += 0xFFF; }
    return nverts;
  }
  nverts = clip_against_plane<2, attribs>(v, nverts, clipstart, far_clip);
  nverts = clip_against_plane<1, attribs>(v, nverts, clipstart, far_clip);
  nverts = clip_against_plane<0, attribs>(v, nverts, clipstart, far_clip);
  return nverts;
}

} // namespace

Gpu3D::Gpu3D(NDS& nds) : nds_(nds), renderer_(nds) { reset(); }

void Gpu3D::reset_render_state() {
  render_count_ = 0;
  rstate_ = RenderState{};
}

void Gpu3D::reset() {
  ring_rd_ = ring_wr_ = pipe_n_ = fifo_n_ = stall_n_ = 0; stalled_ = false;
  num_cmds_ = cur_cmd_ = param_count_ = total_params_ = 0;
  exec_params_.fill(0); exec_count_ = 0;
  timestamp_ = 0; cycle_count_ = 0;
  vertex_pipeline_ = normal_pipeline_ = polygon_pipeline_ = 0;
  vertex_slot_counter_ = 0; vertex_slots_free_ = 1;
  num_pushpop_ = num_tests_ = 0;
  gxstat_ = 0; geometry_on_ = rendering_on_ = false;
  dispcnt_ = 0; alpha_ref_val_ = alpha_ref_ = 0;
  toon_.fill(0); edge_.fill(0);
  fog_color_ = fog_offset_ = 0; fog_density_.fill(0);
  clear_attr1_ = 0x3F000000; clear_attr2_ = 0x00007FFF;
  zero_dot_w_limit_ = 0xFFFFFF;
  reset_render_state();
  render_xpos_ = 0;
  matrix_mode_ = 0;
  mtx_identity(proj_.data()); mtx_identity(pos_.data()); mtx_identity(vec_.data()); mtx_identity(tex_.data());
  clip_dirty_ = true; update_clip_matrix();
  proj_stack_.fill(0); tex_stack_.fill(0);
  for (auto& m : pos_stack_) m.fill(0);
  for (auto& m : vec_stack_) m.fill(0);
  proj_sp_ = pos_sp_ = tex_sp_ = 0;
  viewport_.fill(0);
  poly_mode_ = 0;
  std::memset(cur_vertex_, 0, sizeof cur_vertex_); std::memset(vertex_color_, 0, sizeof vertex_color_);
  std::memset(texcoords_, 0, sizeof texcoords_); std::memset(raw_texcoords_, 0, sizeof raw_texcoords_);
  std::memset(normal_, 0, sizeof normal_);
  std::memset(light_dir_, 0, sizeof light_dir_); std::memset(spec_recip_, 0, sizeof spec_recip_);
  std::memset(light_color_, 0, sizeof light_color_);
  std::memset(mat_diffuse_, 0, sizeof mat_diffuse_); std::memset(mat_ambient_, 0, sizeof mat_ambient_);
  std::memset(mat_specular_, 0, sizeof mat_specular_); std::memset(mat_emission_, 0, sizeof mat_emission_);
  use_shininess_ = false; shininess_.fill(0);
  polygon_attr_ = cur_polygon_attr_ = 0; texparam_ = texpal_ = 0;
  std::memset(pos_test_, 0, sizeof pos_test_); std::memset(vec_test_, 0, sizeof vec_test_);
  std::memset(temp_vtx_, 0, sizeof temp_vtx_);
  vertex_num_ = vertex_in_poly_ = consecutive_polys_ = 0;
  last_strip_poly_ = nullptr; num_opaque_ = 0;
  bank_ = 0; num_vertices_ = num_polygons_ = 0;
  flush_request_ = flush_attr_ = 0; render_identical_ = false;
  renderer_.reset();
}

void Gpu3D::set_powcnt(u16 value) {
  geometry_on_ = value & (1 << 3);
  rendering_on_ = value & (1 << 2);
  if (!rendering_on_) reset_render_state();
}

// ---- timing -------------------------------------------------------------------

void Gpu3D::add_cycles(s32 n) {
  cycle_count_ += n;
  if (vertex_pipeline_ > 0) vertex_pipeline_ = vertex_pipeline_ > n ? vertex_pipeline_ - n : 0;
  if (polygon_pipeline_ > 0) {
    if (polygon_pipeline_ > n) {
      polygon_pipeline_ -= n;
      vertex_slot_counter_ += n;
      while (vertex_slot_counter_ > 9) { vertex_slot_counter_ -= 9; vertex_slots_free_ >>= 1; }
    } else {
      polygon_pipeline_ = 0; vertex_slot_counter_ = 0; vertex_slots_free_ = 1;
    }
  }
}

// A vertex submitted while a polygon is being set up waits for the next free
// 9-cycle slot.
void Gpu3D::next_vertex_slot() {
  s32 n = (9 - vertex_slot_counter_) + 1;
  for (;;) {
    cycle_count_ += n;
    if (vertex_pipeline_ > 0) vertex_pipeline_ = vertex_pipeline_ > n ? vertex_pipeline_ - n : 0;
    if (polygon_pipeline_ > 0) {
      if (polygon_pipeline_ > n) {
        polygon_pipeline_ -= n;
        vertex_slot_counter_ = 1;
        vertex_slots_free_ >>= 1;
        if (vertex_slots_free_ & 1) { vertex_slots_free_ &= ~1u; break; }
        n = 9; continue;
      }
      polygon_pipeline_ = 0; vertex_slot_counter_ = 0; vertex_slots_free_ = 1;
      break;
    }
    break;
  }
}

void Gpu3D::stall_polygon_pipeline(s32 delay, s32 nonstall_delay) {
  if (polygon_pipeline_ > 0) {
    cycle_count_ += polygon_pipeline_ + delay;
    vertex_pipeline_ = 0; normal_pipeline_ = 0;
    polygon_pipeline_ = 0; vertex_slot_counter_ = 0; vertex_slots_free_ = 1;
  } else if (vertex_pipeline_ > nonstall_delay) add_cycles((vertex_pipeline_ - nonstall_delay) + 1);
  else add_cycles(normal_pipeline_ + 1);
}

void Gpu3D::vtx_cmd_submit() {          // vertex commands
  if (!(vertex_slots_free_ & 1)) next_vertex_slot(); else add_cycles(1);
  normal_pipeline_ = 0;
}
void Gpu3D::vtx_cmd_delayed6() {        // may run 6 cycles after a vertex
  if (vertex_pipeline_ > 2) add_cycles((vertex_pipeline_ - 2) + 1); else add_cycles(normal_pipeline_ + 1);
  normal_pipeline_ = 0;
}
void Gpu3D::vtx_cmd_delayed8() {        // may run 8 cycles after a vertex
  if (vertex_pipeline_ > 0) add_cycles(vertex_pipeline_ + 1); else add_cycles(normal_pipeline_ + 1);
  normal_pipeline_ = 0;
}
void Gpu3D::vtx_cmd_delayed4() {        // everything else: 4 cycles after a vertex
  add_cycles(normal_pipeline_ + 1);
  normal_pipeline_ = 0;
}

void Gpu3D::finish_work(s32 cycles) {
  add_cycles(cycles);
  if (normal_pipeline_) normal_pipeline_ -= std::min(normal_pipeline_, cycles);
  cycle_count_ = 0;
  if (vertex_pipeline_ || normal_pipeline_ || polygon_pipeline_) return;
  gxstat_ &= ~(1u << 27);
}

void Gpu3D::run_to_slow(u64 arm9_time) {
  prof::add(prof::C_GX_RUN_SLOW, 1);
  const u64 now = arm9_time >> 1;
  cycle_count_ -= static_cast<s32>(now - timestamp_);
  timestamp_ = now;
  if (cycle_count_ <= 0) {
    if (prof::enabled && pipe_n_) prof::add(prof::C_GX_RUN_SLOW_EXEC, 1);
    while (cycle_count_ <= 0 && pipe_n_) {
      // Both clears are no-ops unless a busy bit is set, which is rare
      // between matrix-stack and test commands: one test covers them.
      if (gxstat_ & ((1u << 14) | (1u << 0))) {
        if (num_pushpop_ == 0) gxstat_ &= ~(1u << 14);
        if (num_tests_ == 0) gxstat_ &= ~(1u << 0);
      }
      execute();
    }
  }
  if (cycle_count_ <= 0 && pipe_n_ == 0) {
    if (gxstat_ & (1u << 27)) finish_work(-cycle_count_); else cycle_count_ = 0;
    if (num_pushpop_ == 0) gxstat_ &= ~(1u << 14);
    if (num_tests_ == 0) gxstat_ &= ~(1u << 0);
  }
}

// ---- FIFO ---------------------------------------------------------------------

void Gpu3D::fifo_write(const Entry& e) {
  // Order of tests follows frequency: a frame is tens of thousands of words
  // into a FIFO that is neither empty nor full.
  if (fifo_n_ - 1 < FIFO_DEPTH - 1) { ring_push(e); ++fifo_n_; }              // 1 <= fifo_n_ < 256
  else if (fifo_n_ == 0 && pipe_n_ < PIPE_DEPTH) { ring_push(e); ++pipe_n_; }
  else if (fifo_n_ == FIFO_DEPTH) {
    // The CPU stalls until the FIFO drains; writes already in flight (an
    // STM's remaining registers) queue up behind it.
    if (stall_n_ == STALL_DEPTH) { static int n = 0; if (n++ < 8) std::fprintf(stderr, "[gx] stall queue overflow: fifo %u running %d dma %d stalled %d now %llu\n", fifo_n_, nds_.sched.running() ? (nds_.sched.running()->which == Cpu::ARM9 ? 9 : 7) : 0, nds_.sched.in_dma(), stalled_, (unsigned long long)nds_.sched.now()); return; }
    ring_push(e); ++stall_n_;
    if (!stalled_) { stalled_ = true; nds_.sched.gx_fifo_full(); }
    return;
  } else { ring_push(e); ++fifo_n_; }                                          // FIFO empty, pipe full
  note_enqueued(e.cmd);
}

Gpu3D::Entry Gpu3D::fifo_read() {
  const Entry e = ring_[ring_rd_];
  ring_rd_ = (ring_rd_ + 1) & (RING - 1);
  --pipe_n_;
  if (pipe_n_ <= 2) {
    // Refill the pipe with up to two FIFO entries: a count move, the entries
    // are already in order behind it.
    const u32 k = fifo_n_ < 2 ? fifo_n_ : 2;
    pipe_n_ += k; fifo_n_ -= k;
    if (stall_n_) {
      // Stalled writes enter the FIFO (or the pipe, if it has room) now, with
      // the status side effects they were denied when they arrived.
      u32 idx = (ring_rd_ + pipe_n_ + fifo_n_) & (RING - 1);
      while (stall_n_ && fifo_n_ < FIFO_DEPTH) {
        if (fifo_n_ == 0 && pipe_n_ < PIPE_DEPTH) ++pipe_n_; else ++fifo_n_;
        --stall_n_;
        note_enqueued(ring_[idx].cmd);
        idx = (idx + 1) & (RING - 1);
      }
      if (stall_n_ == 0) stalled_ = false;
    }
    check_fifo_dma();
    check_fifo_irq_fast();
  }
  return e;
}

void Gpu3D::check_fifo_irq() {
  bool irq = false;
  switch (gxstat_ >> 30) {
  case 1: irq = fifo_n_ < 128; break;
  case 2: irq = fifo_n_ == 0; break;
  default: break;
  }
  nds_.io.set_irq_line(Cpu::ARM9, io::IRQ_GX_FIFO, irq);
}

void Gpu3D::check_fifo_dma() {
  if (fifo_n_ < 128 && nds_.dma.gx_armed()) nds_.dma.check(Cpu::ARM9, dma::MODE9_GXFIFO);
}

// Packed command port: up to four command bytes followed by their parameters.
void Gpu3D::gxfifo_write(u32 value) {
  if (num_cmds_ != 0) {
    // A parameter that does not complete its command: the common word (a
    // 16-parameter matrix load, a vertex pair). Everything else takes the
    // packed-command walk below.
    if (++param_count_ < total_params_) { fifo_write(Entry{value, static_cast<u8>(cur_cmd_)}); return; }
    // The parameter completes its command. When no packed command follows
    // (the usual case: one command per word, 40 k a frame on Golden Sun),
    // the walk below would only shift zero bytes out; finish here.
    fifo_write(Entry{value, static_cast<u8>(cur_cmd_)});
    cur_cmd_ >>= 8; --num_cmds_;
    if (cur_cmd_ == 0) { num_cmds_ = 0; return; }
    param_count_ = 0;
    total_params_ = CMD_PARAMS[cur_cmd_ & 0xFF];
    if (total_params_ > 0) return;
    // Zero-parameter commands packed behind it: the walk enqueues them.
  } else {
    num_cmds_ = 4; cur_cmd_ = value; param_count_ = 0;
    total_params_ = CMD_PARAMS[cur_cmd_ & 0xFF];
    if (total_params_ > 0) return;
  }
  for (;;) {
    if ((cur_cmd_ & 0xFF) || (num_cmds_ == 4 && cur_cmd_ == 0))
      fifo_write(Entry{value, static_cast<u8>(cur_cmd_ & 0xFF)});
    if (param_count_ >= total_params_) {
      cur_cmd_ >>= 8;
      if (--num_cmds_ == 0) break;
      param_count_ = 0;
      total_params_ = CMD_PARAMS[cur_cmd_ & 0xFF];
    }
    if (param_count_ < total_params_) break;
  }
}

// ---- command execution --------------------------------------------------------

void Gpu3D::execute() {
  const Entry e = fifo_read();
  const u32 needed = CMD_PARAMS[e.cmd];
  if (needed <= 1) { exec_single(e.cmd, e.param); return; }
  exec_params_[exec_count_++] = e.param;
  if (exec_count_ == 1) {
    switch (e.cmd) {
    case 0x23: vtx_cmd_submit(); break;
    case 0x34: case 0x71: vtx_cmd_delayed8(); break;
    case 0x70: stall_polygon_pipeline(10 + 1, 0); break;
    default: vtx_cmd_delayed4(); break;
    }
  } else {
    add_cycles(1);
    if (exec_count_ >= needed) { exec_count_ = 0; exec_multi(e.cmd); }
  }
}

void Gpu3D::exec_single(u8 cmd, u32 param) {
  switch (cmd) {
  case 0x10: vtx_cmd_delayed4(); matrix_mode_ = param & 3; break;
  case 0x11:   // push
    vtx_cmd_delayed4(); --num_pushpop_;
    if (matrix_mode_ == 0) {
      if (proj_sp_ > 0) gxstat_ |= (1u << 15);
      proj_stack_ = proj_; proj_sp_ = (proj_sp_ + 1) & 1;
    } else if (matrix_mode_ == 3) {
      if (tex_sp_ > 0) gxstat_ |= (1u << 15);
      tex_stack_ = tex_; tex_sp_ = (tex_sp_ + 1) & 1;
    } else {
      if (pos_sp_ > 30) gxstat_ |= (1u << 15);
      pos_stack_[pos_sp_ & 0x1F] = pos_; vec_stack_[pos_sp_ & 0x1F] = vec_;
      pos_sp_ = (pos_sp_ + 1) & 0x3F;
    }
    add_cycles(16);
    break;
  case 0x12:   // pop
    vtx_cmd_delayed4(); --num_pushpop_;
    if (matrix_mode_ == 0) {
      if (proj_sp_ == 0) gxstat_ |= (1u << 15);
      proj_sp_ = (proj_sp_ - 1) & 1; proj_ = proj_stack_; clip_dirty_ = true;
      add_cycles(35);
    } else if (matrix_mode_ == 3) {
      if (tex_sp_ == 0) gxstat_ |= (1u << 15);
      tex_sp_ = (tex_sp_ - 1) & 1; tex_ = tex_stack_;
      add_cycles(17);
    } else {
      const s32 off = static_cast<s32>(param << 26) >> 26;
      pos_sp_ = (pos_sp_ - off) & 0x3F;
      if (pos_sp_ > 30) gxstat_ |= (1u << 15);
      pos_ = pos_stack_[pos_sp_ & 0x1F]; vec_ = vec_stack_[pos_sp_ & 0x1F]; clip_dirty_ = true;
      add_cycles(35);
    }
    break;
  case 0x13:   // store
    vtx_cmd_delayed4();
    if (matrix_mode_ == 0) proj_stack_ = proj_;
    else if (matrix_mode_ == 3) tex_stack_ = tex_;
    else {
      const u32 a = param & 0x1F;
      if (a > 30) gxstat_ |= (1u << 15);
      pos_stack_[a] = pos_; vec_stack_[a] = vec_;
    }
    add_cycles(16);
    break;
  case 0x14:   // restore
    vtx_cmd_delayed4();
    if (matrix_mode_ == 0) { proj_ = proj_stack_; clip_dirty_ = true; add_cycles(35); }
    else if (matrix_mode_ == 3) { tex_ = tex_stack_; add_cycles(17); }
    else {
      const u32 a = param & 0x1F;
      if (a > 30) gxstat_ |= (1u << 15);
      pos_ = pos_stack_[a]; vec_ = vec_stack_[a]; clip_dirty_ = true;
      add_cycles(35);
    }
    break;
  case 0x15:   // identity
    vtx_cmd_delayed4();
    if (matrix_mode_ == 0) { mtx_identity(proj_.data()); clip_dirty_ = true; add_cycles(18); }
    else if (matrix_mode_ == 3) mtx_identity(tex_.data());
    else { mtx_identity(pos_.data()); if (matrix_mode_ == 2) mtx_identity(vec_.data()); clip_dirty_ = true; add_cycles(18); }
    break;
  case 0x20:   // colour
    vtx_cmd_delayed6();
    vertex_color_[0] = param & 0x1F; vertex_color_[1] = (param >> 5) & 0x1F; vertex_color_[2] = (param >> 10) & 0x1F;
    break;
  case 0x21:   // normal
    vtx_cmd_delayed4();
    normal_[0] = sext10(param & 0x3FF); normal_[1] = sext10((param >> 10) & 0x3FF); normal_[2] = sext10((param >> 20) & 0x3FF);
    calculate_lighting();
    break;
  case 0x22:   // texcoord
    vtx_cmd_delayed4();
    raw_texcoords_[0] = static_cast<s16>(param & 0xFFFF); raw_texcoords_[1] = static_cast<s16>(param >> 16);
    if ((texparam_ >> 30) == 1) {
      texcoords_[0] = static_cast<s16>((raw_texcoords_[0] * tex_[0] + raw_texcoords_[1] * tex_[4] + tex_[8] + tex_[12]) >> 12);
      texcoords_[1] = static_cast<s16>((raw_texcoords_[0] * tex_[1] + raw_texcoords_[1] * tex_[5] + tex_[9] + tex_[13]) >> 12);
    } else { texcoords_[0] = raw_texcoords_[0]; texcoords_[1] = raw_texcoords_[1]; }
    break;
  case 0x24:   // 10-bit vertex
    vtx_cmd_submit();
    cur_vertex_[0] = static_cast<s16>((param & 0x3FF) << 6); cur_vertex_[1] = static_cast<s16>((param & 0xFFC00) >> 4); cur_vertex_[2] = static_cast<s16>((param & 0x3FF00000) >> 14);
    submit_vertex();
    break;
  case 0x25: vtx_cmd_submit(); cur_vertex_[0] = static_cast<s16>(param & 0xFFFF); cur_vertex_[1] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x26: vtx_cmd_submit(); cur_vertex_[0] = static_cast<s16>(param & 0xFFFF); cur_vertex_[2] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x27: vtx_cmd_submit(); cur_vertex_[1] = static_cast<s16>(param & 0xFFFF); cur_vertex_[2] = static_cast<s16>(param >> 16); submit_vertex(); break;
  case 0x28:   // delta vertex
    vtx_cmd_submit();
    cur_vertex_[0] = static_cast<s16>(cur_vertex_[0] + sext10(param & 0x3FF));
    cur_vertex_[1] = static_cast<s16>(cur_vertex_[1] + sext10((param >> 10) & 0x3FF));
    cur_vertex_[2] = static_cast<s16>(cur_vertex_[2] + sext10((param >> 20) & 0x3FF));
    submit_vertex();
    break;
  case 0x29: vtx_cmd_delayed8(); polygon_attr_ = param; break;
  case 0x2A: vtx_cmd_delayed8(); texparam_ = param; break;
  case 0x2B: vtx_cmd_delayed8(); texpal_ = param & 0x1FFF; break;
  case 0x30:   // diffuse / ambient
    vtx_cmd_delayed6();
    mat_diffuse_[0] = param & 0x1F; mat_diffuse_[1] = (param >> 5) & 0x1F; mat_diffuse_[2] = (param >> 10) & 0x1F;
    mat_ambient_[0] = (param >> 16) & 0x1F; mat_ambient_[1] = (param >> 21) & 0x1F; mat_ambient_[2] = (param >> 26) & 0x1F;
    if (param & 0x8000) { vertex_color_[0] = mat_diffuse_[0]; vertex_color_[1] = mat_diffuse_[1]; vertex_color_[2] = mat_diffuse_[2]; }
    add_cycles(3);
    break;
  case 0x31:   // specular / emission
    vtx_cmd_delayed6();
    mat_specular_[0] = param & 0x1F; mat_specular_[1] = (param >> 5) & 0x1F; mat_specular_[2] = (param >> 10) & 0x1F;
    mat_emission_[0] = (param >> 16) & 0x1F; mat_emission_[1] = (param >> 21) & 0x1F; mat_emission_[2] = (param >> 26) & 0x1F;
    use_shininess_ = (param & 0x8000) != 0;
    add_cycles(3);
    break;
  case 0x32: {  // light vector
    stall_polygon_pipeline(8 + 1, 2);
    const u32 l = param >> 30;
    const s16 d0 = sext10(param & 0x3FF), d1 = sext10((param >> 10) & 0x3FF), d2 = sext10((param >> 20) & 0x3FF);
    // Transformed by the vector matrix; the low 12 bits go before the
    // negation and the result is kept as a signed 11-bit value.
    auto s11 = [](s32 v) { return static_cast<s16>(static_cast<s32>(static_cast<u32>(v) << 21) >> 21); };
    light_dir_[l][0] = s11(-((d0 * vec_[0] + d1 * vec_[4] + d2 * vec_[8]) >> 12));
    light_dir_[l][1] = s11(-((d0 * vec_[1] + d1 * vec_[5] + d2 * vec_[9]) >> 12));
    light_dir_[l][2] = s11(-((d0 * vec_[2] + d1 * vec_[6] + d2 * vec_[10]) >> 12));
    const s32 den = -((static_cast<s32>(static_cast<u32>(d0 * vec_[2] + d1 * vec_[6] + d2 * vec_[10]) << 9)) >> 21) + (1 << 9);
    spec_recip_[l] = den == 0 ? 0 : (1 << 18) / den;
    add_cycles(5);
    break;
  }
  case 0x33: {  // light colour
    vtx_cmd_delayed8();
    const u32 l = param >> 30;
    light_color_[l][0] = param & 0x1F; light_color_[l][1] = (param >> 5) & 0x1F; light_color_[l][2] = (param >> 10) & 0x1F;
    add_cycles(1);
    break;
  }
  case 0x40:   // begin
    stall_polygon_pipeline(1, 0);
    poly_mode_ = param & 3;
    vertex_num_ = 0; vertex_in_poly_ = 0; consecutive_polys_ = 0;
    last_strip_poly_ = nullptr;
    cur_polygon_attr_ = polygon_attr_;
    break;
  case 0x41: vtx_cmd_delayed8(); break;   // end: no effect
  case 0x50:   // swap buffers
    vtx_cmd_delayed4();
    flush_request_ = 1; flush_attr_ = param & 3;
    cycle_count_ = 325;
    vertex_pipeline_ = normal_pipeline_ = polygon_pipeline_ = 0;
    vertex_slot_counter_ = 0; vertex_slots_free_ = 1;
    break;
  case 0x60:   // viewport (Y is upside down)
    vtx_cmd_delayed8();
    viewport_[0] = param & 0xFF;
    viewport_[1] = (191 - ((param >> 8) & 0xFF)) & 0xFF;
    viewport_[2] = (param >> 16) & 0xFF;
    viewport_[3] = (191 - (param >> 24)) & 0xFF;
    viewport_[4] = (viewport_[2] - viewport_[0] + 1) & 0x1FF;
    viewport_[5] = (viewport_[1] - viewport_[3] + 1) & 0xFF;
    break;
  case 0x72: vtx_cmd_delayed6(); --num_tests_; vec_test(param); break;
  default: vtx_cmd_delayed4(); break;
  }
}

void Gpu3D::exec_multi(u8 cmd) {
  const s32* p = reinterpret_cast<const s32*>(exec_params_.data());
  auto on_matrix = [&](auto&& op, s32 proj_cycles, s32 tex_cycles, s32 pos_cycles, s32 posvec_cycles) {
    if (matrix_mode_ == 0) { op(proj_.data()); clip_dirty_ = true; add_cycles(proj_cycles); }
    else if (matrix_mode_ == 3) { op(tex_.data()); add_cycles(tex_cycles); }
    else {
      op(pos_.data());
      if (matrix_mode_ == 2) { op(vec_.data()); add_cycles(posvec_cycles); } else add_cycles(pos_cycles);
      clip_dirty_ = true;
    }
  };
  switch (cmd) {
  case 0x16: on_matrix([&](s32* m) { std::memcpy(m, p, 64); }, 18, 10, 18, 18); break;
  case 0x17: on_matrix([&](s32* m) { mtx_load_4x3(m, p); }, 18, 7, 18, 18); break;
  case 0x18: on_matrix([&](s32* m) { mtx_mult_4x4(m, p); }, 35 - 16, 33 - 16, 35 - 16, 35 + 30 - 16); break;
  case 0x19: on_matrix([&](s32* m) { mtx_mult_4x3(m, p); }, 35 - 12, 33 - 12, 35 - 12, 35 + 30 - 12); break;
  case 0x1A: on_matrix([&](s32* m) { mtx_mult_3x3(m, p); }, 35 - 9, 33 - 9, 35 - 9, 35 + 30 - 9); break;
  case 0x1B:   // scale: never applied to the vector matrix
    if (matrix_mode_ == 0) { mtx_scale(proj_.data(), p); clip_dirty_ = true; add_cycles(35 - 3); }
    else if (matrix_mode_ == 3) { mtx_scale(tex_.data(), p); add_cycles(33 - 3); }
    else { mtx_scale(pos_.data(), p); clip_dirty_ = true; add_cycles(35 - 3); }
    break;
  case 0x1C: on_matrix([&](s32* m) { mtx_translate(m, p); }, 35 - 3, 33 - 3, 35 - 3, 35 + 30 - 3); break;
  case 0x23:   // full vertex
    cur_vertex_[0] = static_cast<s16>(exec_params_[0] & 0xFFFF); cur_vertex_[1] = static_cast<s16>(exec_params_[0] >> 16);
    cur_vertex_[2] = static_cast<s16>(exec_params_[1] & 0xFFFF);
    submit_vertex();
    break;
  case 0x34:   // shininess table
    for (int i = 0; i < 128; i += 4) {
      const u32 v = exec_params_[i >> 2];
      shininess_[i] = v & 0xFF; shininess_[i + 1] = (v >> 8) & 0xFF; shininess_[i + 2] = (v >> 16) & 0xFF; shininess_[i + 3] = v >> 24;
    }
    break;
  case 0x71:   // position test
    num_tests_ -= 2;
    cur_vertex_[0] = static_cast<s16>(exec_params_[0] & 0xFFFF); cur_vertex_[1] = static_cast<s16>(exec_params_[0] >> 16);
    cur_vertex_[2] = static_cast<s16>(exec_params_[1] & 0xFFFF);
    pos_test();
    break;
  case 0x70: num_tests_ -= 3; box_test(exec_params_.data()); break;
  default: break;
  }
}

// ---- geometry -----------------------------------------------------------------

void Gpu3D::update_clip_matrix() {
  if (!clip_dirty_) return;
  clip_dirty_ = false;
  clip_ = proj_;
  mtx_mult_4x4(clip_.data(), pos_.data());
}

void Gpu3D::submit_vertex() {
  const s64 v[4] = {cur_vertex_[0], cur_vertex_[1], cur_vertex_[2], 0x1000};
  Vertex& vt = temp_vtx_[vertex_in_poly_];
  update_clip_matrix();
  for (int c = 0; c < 4; ++c)
    vt.pos[c] = static_cast<s32>((v[0] * clip_[c] + v[1] * clip_[4 + c] + v[2] * clip_[8 + c] + v[3] * clip_[12 + c]) >> 12);
  for (int c = 0; c < 3; ++c) vt.col[c] = (vertex_color_[c] << 12) + 0xFFF;
  if ((texparam_ >> 30) == 3) {
    vt.tex[0] = static_cast<s16>(((v[0] * tex_[0] + v[1] * tex_[4] + v[2] * tex_[8]) >> 24) + raw_texcoords_[0]);
    vt.tex[1] = static_cast<s16>(((v[0] * tex_[1] + v[1] * tex_[5] + v[2] * tex_[9]) >> 24) + raw_texcoords_[1]);
  } else { vt.tex[0] = texcoords_[0]; vt.tex[1] = texcoords_[1]; }
  vt.clipped = false;

  ++vertex_num_; ++vertex_in_poly_;
  switch (poly_mode_) {
  case 0: if (vertex_in_poly_ == 3) { vertex_in_poly_ = 0; submit_polygon(); ++consecutive_polys_; } break;
  case 1: if (vertex_in_poly_ == 4) { vertex_in_poly_ = 0; submit_polygon(); ++consecutive_polys_; } break;
  case 2:   // triangle strip
    if (consecutive_polys_ & 1) {
      std::swap(temp_vtx_[0], temp_vtx_[1]);
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      temp_vtx_[1] = temp_vtx_[2];
    } else if (vertex_in_poly_ == 3) {
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      temp_vtx_[0] = temp_vtx_[1]; temp_vtx_[1] = temp_vtx_[2];
    }
    break;
  case 3:   // quad strip
    if (vertex_in_poly_ == 4) {
      std::swap(temp_vtx_[2], temp_vtx_[3]);
      vertex_in_poly_ = 2; submit_polygon(); ++consecutive_polys_;
      temp_vtx_[0] = temp_vtx_[3]; temp_vtx_[1] = temp_vtx_[2];
    }
    break;
  }
  vertex_pipeline_ = 7;
  add_cycles(3);
}

void Gpu3D::submit_polygon() {
  Vertex clipped[10];
  const Vertex* reused[2] = {nullptr, nullptr};
  u16 reused_idx[2] = {0, 0};
  int clipstart = 0, lastpolyverts = 0;
  int nverts = (poly_mode_ & 1) ? 4 : 3;

  // Submitting a polygon starts the polygon pipeline; one vertex slot is
  // reserved now, more once it survives culling and clipping.
  polygon_pipeline_ = 8; vertex_slot_counter_ = 1; vertex_slots_free_ = 0b11110;

  // Culling from the first three vertices' clip-space positions.
  const Vertex &v0 = temp_vtx_[0], &v1 = temp_vtx_[1], &v2 = temp_vtx_[2];
  s64 nx = static_cast<s64>(v0.pos[1] - v1.pos[1]) * (v2.pos[3] - v1.pos[3]) - static_cast<s64>(v0.pos[3] - v1.pos[3]) * (v2.pos[1] - v1.pos[1]);
  s64 ny = static_cast<s64>(v0.pos[3] - v1.pos[3]) * (v2.pos[0] - v1.pos[0]) - static_cast<s64>(v0.pos[0] - v1.pos[0]) * (v2.pos[3] - v1.pos[3]);
  s64 nz = static_cast<s64>(v0.pos[0] - v1.pos[0]) * (v2.pos[1] - v1.pos[1]) - static_cast<s64>(v0.pos[1] - v1.pos[1]) * (v2.pos[0] - v1.pos[0]);
  while ((((nx >> 31) ^ (nx >> 63)) != 0) || (((ny >> 31) ^ (ny >> 63)) != 0) || (((nz >> 31) ^ (nz >> 63)) != 0)) { nx >>= 4; ny >>= 4; nz >>= 4; }
  const s64 dot = static_cast<s64>(v1.pos[0]) * nx + static_cast<s64>(v1.pos[1]) * ny + static_cast<s64>(v1.pos[3]) * nz;
  const bool facing = dot <= 0;
  if (dot < 0) { if (!(cur_polygon_attr_ & (1 << 7))) { last_strip_poly_ = nullptr; return; } }
  else if (dot > 0) { if (!(cur_polygon_attr_ & (1 << 6))) { last_strip_poly_ = nullptr; return; } }

  // Strips share two unclipped vertices with the previous polygon.
  if (poly_mode_ >= 2 && last_strip_poly_) {
    int id0, id1;
    if (poly_mode_ == 2) {
      if (consecutive_polys_ & 1) { id0 = 2; id1 = 1; } else { id0 = 0; id1 = 2; }
      lastpolyverts = 3;
    } else { id0 = 3; id1 = 2; lastpolyverts = 4; }
    if (static_cast<int>(last_strip_poly_->nverts) == lastpolyverts &&
        !vram_[last_strip_poly_->vtx[id0]].clipped && !vram_[last_strip_poly_->vtx[id1]].clipped) {
      reused_idx[0] = last_strip_poly_->vtx[id0]; reused_idx[1] = last_strip_poly_->vtx[id1];
      reused[0] = &vram_[reused_idx[0]]; reused[1] = &vram_[reused_idx[1]];
      clipped[0] = *reused[0]; clipped[1] = *reused[1];
      clipstart = 2;
    }
  }
  for (int i = clipstart; i < nverts; ++i) clipped[i] = temp_vtx_[i];

  nverts = clip_polygon<true>(clipped, nverts, clipstart, cur_polygon_attr_ & (1 << 12));
  if (nverts == 0) { last_strip_poly_ = nullptr; return; }

  if (num_polygons_ >= PRAM_BANK || num_vertices_ + nverts > VRAM_BANK) {
    last_strip_poly_ = nullptr;
    dispcnt_ |= (1 << 13);                     // RAM overflow flag
    return;
  }

  // Viewport transform. W is truncated to 24 bits; the 32-bit divider loses
  // a bit of precision when W exceeds 16 bits.
  for (int i = clipstart; i < nverts; ++i) {
    Vertex& vt = clipped[i];
    vt.pos[3] &= 0x00FFFFFF;
    u32 px, py;
    const u32 w = static_cast<u32>(vt.pos[3]);
    if (w == 0) { px = 0; py = 0; }
    else {
      px = static_cast<u32>(vt.pos[0]) + w;
      py = static_cast<u32>(-vt.pos[1]) + w;
      u32 den = w;
      if (w > 0xFFFF) { px >>= 1; py >>= 1; den >>= 1; }
      den <<= 1;
      px = ((px * viewport_[4]) / den) + viewport_[0];
      py = ((py * viewport_[5]) / den) + viewport_[3];
    }
    vt.sx = px & 0x1FF;
    vt.sy = py & 0xFF;
  }

  // Zero-dot polygons (all vertices on one pixel) beyond the W limit are dropped.
  if (!(cur_polygon_attr_ & (1 << 13))) {
    bool zerodot = true, allbehind = true;
    for (int i = 0; i < nverts; ++i) {
      if (clipped[i].sx != clipped[0].sx || clipped[i].sy != clipped[0].sy) { zerodot = false; break; }
      if (static_cast<u32>(clipped[i].pos[3]) <= zero_dot_w_limit_) { allbehind = false; break; }
    }
    if (zerodot && allbehind) { last_strip_poly_ = nullptr; return; }
  }

  if (nverts == 4) { polygon_pipeline_ = 35; vertex_slot_counter_ = 1; vertex_slots_free_ = (poly_mode_ & 2) ? 0b11100 : 0b11110; }
  else { polygon_pipeline_ = 26; vertex_slot_counter_ = 1; vertex_slots_free_ = (poly_mode_ & 2) ? 0b1000 : 0b1110; }

  Polygon* poly = &cur_pram()[num_polygons_++];
  poly->nverts = 0;
  poly->attr = cur_polygon_attr_; poly->texparam = texparam_; poly->texpal = texpal_;
  poly->degenerate = false;
  poly->facing = facing;
  const u32 texfmt = (texparam_ >> 26) & 7, polyalpha = (cur_polygon_attr_ >> 16) & 0x1F;
  poly->translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);
  poly->shadow_mask = (cur_polygon_attr_ & 0x3F000030) == 0x00000030;
  poly->shadow = ((cur_polygon_attr_ & 0x30) == 0x30) && !poly->shadow_mask;
  if (!poly->translucent) ++num_opaque_;

  Vertex* vr = cur_vram();
  if (last_strip_poly_ && clipstart > 0) {
    if (nverts == lastpolyverts) { poly->vtx[0] = reused_idx[0]; poly->vtx[1] = reused_idx[1]; }
    else {
      vr[num_vertices_] = *reused[0]; poly->vtx[0] = static_cast<u16>(vram_base() + num_vertices_);
      vr[num_vertices_ + 1] = *reused[1]; poly->vtx[1] = static_cast<u16>(vram_base() + num_vertices_ + 1);
      num_vertices_ += 2;
    }
    poly->nverts += 2;
  }
  for (int i = clipstart; i < nverts; ++i) {
    Vertex& vt = vr[num_vertices_];
    vt = clipped[i];
    poly->vtx[i] = static_cast<u16>(vram_base() + num_vertices_);
    ++num_vertices_; ++poly->nverts;
    // 5-bit colour to 9 bits: (c << 4) + 0xF for non-zero components.
    for (int c = 0; c < 3; ++c) { vt.fcol[c] = vt.col[c] >> 12; if (vt.fcol[c]) vt.fcol[c] = (vt.fcol[c] << 4) + 0xF; }
  }

  // Bounds, and the W range used to normalise W to 16 bits.
  u32 vtop = 0, vbot = 0; s32 ytop = 192, ybot = 0, xtop = 256, xbot = 0; u32 wsize = 0;
  for (int i = 0; i < nverts; ++i) {
    const Vertex& vt = vram_[poly->vtx[i]];
    if (vt.sy < ytop) { xtop = vt.sx; ytop = vt.sy; vtop = i; }
    if (vt.sy > ybot || (vt.sy == ybot && vt.sx > xbot)) { xbot = vt.sx; ybot = vt.sy; vbot = i; }
    const u32 w = static_cast<u32>(vt.pos[3]);
    if (w == 0) poly->degenerate = true;
    // Smallest multiple of 4 that shifts w to zero, capped at 32 -- what
    // `while ((w >> wsize) && wsize < 32) wsize += 4` converges to.
    if (w) { const u32 need = (35 - static_cast<u32>(__builtin_clz(w))) & ~3u; if (need > wsize) wsize = need; }
  }
  poly->vtop = vtop; poly->vbot = vbot; poly->ytop = ytop; poly->ybot = ybot; poly->xtop = xtop; poly->xbot = xbot;
  if (ybot > 192) poly->degenerate = true;
  poly->sort_key = (ybot << 8) | ytop;
  if (poly->translucent) poly->sort_key |= 0x10000;
  poly->wbuffer = flush_attr_ & 2;

  for (int i = 0; i < nverts; ++i) {
    const Vertex& vt = vram_[poly->vtx[i]];
    s32 w, wshifted;
    if (wsize < 16) { w = vt.pos[3] << (16 - wsize); wshifted = w >> (16 - wsize); }
    else { w = vt.pos[3] >> (wsize - 16); wshifted = w << (wsize - 16); }
    s32 z;
    if (flush_attr_ & 2) z = wshifted;
    else if (vt.pos[3]) z = static_cast<s32>(((static_cast<s64>(vt.pos[2]) * 0x4000) / vt.pos[3] + 0x3FFF) * 0x200);
    else z = 0x7FFE00;
    if (z < 0) z = 0; else if (z > 0xFFFFFF) z = 0xFFFFFF;
    poly->z[i] = z; poly->w[i] = w;
  }
  last_strip_poly_ = poly_mode_ >= 2 ? poly : nullptr;
}

void Gpu3D::calculate_lighting() {
  if ((texparam_ >> 30) == 2) {
    texcoords_[0] = static_cast<s16>(raw_texcoords_[0] + ((static_cast<s64>(normal_[0]) * tex_[0] + static_cast<s64>(normal_[1]) * tex_[4] + static_cast<s64>(normal_[2]) * tex_[8]) >> 21));
    texcoords_[1] = static_cast<s16>(raw_texcoords_[1] + ((static_cast<s64>(normal_[0]) * tex_[1] + static_cast<s64>(normal_[1]) * tex_[5] + static_cast<s64>(normal_[2]) * tex_[9]) >> 21));
  }
  // Normal through the vector matrix, kept as 1.10 signed.
  s32 n[3];
  for (int c = 0; c < 3; ++c)
    n[c] = static_cast<s32>(static_cast<u32>(normal_[0] * vec_[c] + normal_[1] * vec_[4 + c] + normal_[2] * vec_[8 + c]) << 9) >> 21;

  s32 count = 0;
  u32 acc[3] = {static_cast<u32>(mat_emission_[0]) << 14, static_cast<u32>(mat_emission_[1]) << 14, static_cast<u32>(mat_emission_[2]) << 14};
  for (int i = 0; i < 4; ++i) {
    if (!(cur_polygon_attr_ & (1 << i))) continue;
    s32 dot = ((light_dir_[i][0] * n[0]) >> 9) + ((light_dir_[i][1] * n[1]) >> 9) + ((light_dir_[i][2] * n[2]) >> 9);
    s32 shine;
    if (dot > 0) {
      // Diffuse: dot as signed 11-bit, products truncated to 20 bits.
      const s32 diffdot = static_cast<s32>(static_cast<u32>(dot) << 21) >> 21;
      for (int c = 0; c < 3; ++c) acc[c] += static_cast<u32>(mat_diffuse_[c] * light_color_[i][c] * diffdot) & 0xFFFFF;
      // Specular: half-vector approximation reusing the dot product.
      dot += n[2];
      dot = static_cast<s32>(static_cast<u32>(dot) << 21) >> 21;
      dot = ((dot * dot) >> 10) & 0x3FF;
      shine = ((dot * spec_recip_[i]) >> 8) - (1 << 9);
      if (shine < 0) shine = 0;
      else {
        shine = static_cast<s32>(static_cast<u32>(shine) << 18) >> 18;
        if (shine < 0) shine = 0; else if (shine > 0x1FF) shine = 0x1FF;
      }
    } else shine = 0;
    if (use_shininess_) { shine >>= 2; shine = shininess_[shine]; shine <<= 1; }
    for (int c = 0; c < 3; ++c) acc[c] += ((mat_specular_[c] * shine) + (mat_ambient_[c] << 9)) * light_color_[i][c];
    ++count;
  }
  for (int c = 0; c < 3; ++c) vertex_color_[c] = (acc[c] >> 14) > 31 ? 31 : static_cast<u8>(acc[c] >> 14);
  if (count < 1) count = 1;
  normal_pipeline_ = 7;
  add_cycles(count);
}

void Gpu3D::box_test(const u32* params) {
  add_cycles(254);
  gxstat_ &= ~(1u << 1);
  const s16 x0 = static_cast<s16>(params[0] & 0xFFFF), y0 = static_cast<s16>(static_cast<s32>(params[0]) >> 16);
  const s16 z0 = static_cast<s16>(params[1] & 0xFFFF);
  const s16 x1 = static_cast<s16>(x0 + static_cast<s16>(static_cast<s32>(params[1]) >> 16));
  const s16 y1 = static_cast<s16>(y0 + static_cast<s16>(params[2] & 0xFFFF));
  const s16 z1 = static_cast<s16>(z0 + static_cast<s16>(static_cast<s32>(params[2]) >> 16));
  Vertex cube[8] = {};
  const s16 corners[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}};
  update_clip_matrix();
  for (int i = 0; i < 8; ++i) {
    const s64 x = corners[i][0], y = corners[i][1], z = corners[i][2];
    for (int c = 0; c < 4; ++c)
      cube[i].pos[c] = static_cast<s32>((x * clip_[c] + y * clip_[4 + c] + z * clip_[8 + c] + static_cast<s64>(0x1000) * clip_[12 + c]) >> 12);
  }
  static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 3, 4, 5}, {1, 2, 7, 6}, {0, 1, 6, 5}, {2, 3, 4, 7}};
  for (const auto& f : faces) {
    Vertex face[10];
    for (int i = 0; i < 4; ++i) face[i] = cube[f[i]];
    if (clip_polygon<false>(face, 4, 0, cur_polygon_attr_ & (1 << 12)) > 0) { gxstat_ |= (1u << 1); return; }
  }
}

void Gpu3D::pos_test() {
  const s64 v[4] = {cur_vertex_[0], cur_vertex_[1], cur_vertex_[2], 0x1000};
  update_clip_matrix();
  for (int c = 0; c < 4; ++c)
    pos_test_[c] = static_cast<s32>((v[0] * clip_[c] + v[1] * clip_[4 + c] + v[2] * clip_[8 + c] + v[3] * clip_[12 + c]) >> 12);
  add_cycles(5);
}

void Gpu3D::vec_test(u32 param) {
  const s16 n[3] = {sext10(param & 0x3FF), sext10((param >> 10) & 0x3FF), sext10((param >> 20) & 0x3FF)};
  for (int c = 0; c < 3; ++c) {
    vec_test_[c] = static_cast<s16>((n[0] * vec_[c] + n[1] * vec_[4 + c] + n[2] * vec_[8 + c]) >> 9);
    if (vec_test_[c] & 0x1000) vec_test_[c] |= static_cast<s16>(0xF000);
  }
  add_cycles(4);
}

// ---- frame --------------------------------------------------------------------

void Gpu3D::vblank() {
  if (std::getenv("DS_DEBUG_GX"))
    std::fprintf(stderr, "[gx] frame %llu geom %d rend %d flush %u attr %u polys %u verts %u disp3dcnt %04x alpharef %u clear %08x/%08x fifo %u gxstat %08x ie %08x if %08x\n",
                 static_cast<unsigned long long>(nds_.frame_count), geometry_on_, rendering_on_, flush_request_, flush_attr_, num_polygons_, num_vertices_,
                 dispcnt_, alpha_ref_, clear_attr1_, clear_attr2_, fifo_n_, gxstat_, nds_.io.cpu_io[0].ie, nds_.io.cpu_io[0].if_);
  if (!geometry_on_) return;
  if (rendering_on_) {
    // The render registers this frame against the ones the last render used.
    // Both the no-swap path and the duplicate-list skip need this answer.
    const bool same_disp  = rstate_.dispcnt == dispcnt_ && rstate_.alpha_ref == alpha_ref_;
    const bool same_clear = rstate_.clear_attr1 == clear_attr1_ && rstate_.clear_attr2 == clear_attr2_;
    const bool same_fog   = rstate_.fog_color == fog_color_ && rstate_.fog_offset == fog_offset_ * 0x200u
      && std::equal(fog_density_.begin(), fog_density_.end(), rstate_.fog_density.begin() + 1);
    const bool same_et    = rstate_.edge == edge_ && rstate_.toon == toon_;
    const bool same_regs  = same_disp && same_clear && same_fog && same_et;
    if (flush_request_) {
      if (num_polygons_) {
        // Opaque polygons first, then translucent; each group sorted by
        // bottom Y then top Y (stable), unless the flush asked for manual
        // translucent ordering.
        u32 io = 0, it = num_opaque_;
        const Polygon* pr = cur_pram();
        for (u32 i = 0; i < num_polygons_; ++i) { const Polygon* p = &pr[i]; if (p->translucent) render_polys_[it++] = p; else render_polys_[io++] = p; }
        std::stable_sort(render_polys_.begin(), render_polys_.begin() + ((flush_attr_ & 1) ? num_opaque_ : num_polygons_),
                         [](const Polygon* a, const Polygon* b) { return a->sort_key < b->sort_key; });
      }
      render_count_ = num_polygons_;
      // A swap that resubmits the same geometry with the same render state
      // produces the same picture: keep the previous output (the rasteriser
      // still checks its textures itself).
      render_identical_ = skip_dup() && same_regs && rendered_before_
        && num_polygons_ == prev_swap_polys_ && num_vertices_ == prev_swap_verts_
        && lists_equal(&pram_[bank_ * PRAM_BANK], &pram_[(bank_ ^ 1) * PRAM_BANK], num_polygons_,
                       bank_ * VRAM_BANK, (bank_ ^ 1) * VRAM_BANK, vram_.data());
      prev_swap_polys_ = num_polygons_; prev_swap_verts_ = num_vertices_; rendered_before_ = true;
      if (prof::enabled) {
        prof::add(prof::C_GX_SWAP, 1);
        // Sums, plus a running max kept by adding the shortfall (vblank is
        // always the emulation thread, so this accumulator is the only one).
        prof::add(prof::C_GX_SWAP_POLYS, num_polygons_);
        prof::add(prof::C_GX_SWAP_VERTS, num_vertices_);
        if (num_polygons_ > prof::count(prof::C_GX_SWAP_MAXPOLYS))
          prof::add(prof::C_GX_SWAP_MAXPOLYS, num_polygons_ - prof::count(prof::C_GX_SWAP_MAXPOLYS));
        if (num_vertices_ > prof::count(prof::C_GX_SWAP_MAXVERTS))
          prof::add(prof::C_GX_SWAP_MAXVERTS, num_vertices_ - prof::count(prof::C_GX_SWAP_MAXVERTS));
        if (census_gx()) {
          const u64 h = census_list_hash(render_polys_.data(), render_count_, vram_.data());
          prof::census_same_list = census_have_prev_ && h == census_prev_hash_;
          if (prof::census_same_list) {
            prof::add(prof::C_GX_SWAP_SAME_CONTENT, 1);
            // How much geometry is actually in the frames we could skip?
            prof::add(prof::C_GX_SAME_POLYS, num_polygons_);
            prof::add(prof::C_GX_SAME_VERTS, num_vertices_);
          }
          census_prev_hash_ = h; census_have_prev_ = true;
          // The compare alternative, on the same frames.
          if (census_have_prev_counts_ && num_polygons_ == census_prev_polys_ && num_vertices_ == census_prev_verts_) {
            const u32 other = bank_ ^ 1;
            const CmpModel m = census_compare(&pram_[bank_ * PRAM_BANK], &pram_[other * PRAM_BANK], num_polygons_,
                                              bank_ * VRAM_BANK, other * VRAM_BANK, vram_.data(), vram_.data());
            prof::add(prof::C_GX_CMP_RUNS, 1);
            prof::add(prof::C_GX_CMP_FULL, m.full);
            prof::add(prof::C_GX_CMP_EARLY, m.early);
            // Split by outcome: an identical list must be scanned in full, a
            // differing one stops early -- averaging the two hides both.
            if (m.early == m.full) { prof::add(prof::C_GX_CMP_RUNS_SAME, 1); prof::add(prof::C_GX_CMP_FULL_SAME, m.full); }
            else { prof::add(prof::C_GX_CMP_RUNS_DIFF, 1); prof::add(prof::C_GX_CMP_FULL_DIFF, m.full); prof::add(prof::C_GX_CMP_EARLY_DIFF, m.early); }
          }
          census_prev_polys_ = num_polygons_; census_prev_verts_ = num_vertices_; census_have_prev_counts_ = true;
        }
      }
    } else {
      // Same polygon list as last time; identical output if the render
      // registers match what that render used (melonDS's RenderFrameIdentical).
      // Split by register group so the census can say which one rejects a
      // frame -- the conjunction is unchanged, only its short-circuiting is.
      render_identical_ = same_regs;
      if (prof::enabled) {
        prof::add(prof::C_GX_NOSWAP, 1);
        if (!render_identical_) {
          prof::add(prof::C_GX_NOSWAP_REGS_DIFFER, 1);
          if (!same_disp)  prof::add(prof::C_GX_RD_DISPCNT, 1);
          if (!same_clear) prof::add(prof::C_GX_RD_CLEAR, 1);
          if (!same_fog)   prof::add(prof::C_GX_RD_FOG, 1);
          if (!same_et)    prof::add(prof::C_GX_RD_EDGETOON, 1);
        }
      }
    }
    rstate_.dispcnt = dispcnt_;
    rstate_.alpha_ref = alpha_ref_;
    rstate_.edge = edge_; rstate_.toon = toon_;
    rstate_.fog_color = fog_color_;
    rstate_.fog_offset = fog_offset_ * 0x200;
    rstate_.fog_shift = (dispcnt_ >> 8) & 0xF;
    rstate_.fog_density[0] = fog_density_[0];
    for (int i = 0; i < 32; ++i) rstate_.fog_density[i + 1] = fog_density_[i];
    rstate_.fog_density[33] = fog_density_[31];
    rstate_.clear_attr1 = clear_attr1_; rstate_.clear_attr2 = clear_attr2_;
  }
  if (flush_request_) {
    bank_ ^= 1;
    num_vertices_ = num_polygons_ = num_opaque_ = 0;
    flush_request_ = 0;
  }
}

void Gpu3D::render_frame() { renderer_.render(*this); }

void Gpu3D::set_render_xpos(u16 value, u16 mask) {
  if (!rendering_on_) return;
  render_xpos_ = (render_xpos_ & ~mask) | (value & mask & 0x1FF);
}

void Gpu3D::sync_raster() { renderer_.sync_all(); }

const u32* Gpu3D::line(u32 y) {
  renderer_.sync_line(static_cast<s32>(y));
  const u32* raw = renderer_.raw_line(y);
  const u32 xpos = render_xpos_;
  if (xpos == 0) return raw;
  static u32 scrolled[256];
  if (xpos & 0x100) {
    u32 i = 0, j = xpos;
    for (; j < 512; ++i, ++j) scrolled[i] = 0;
    for (j = 0; i < 256; ++i, ++j) scrolled[i] = raw[j];
  } else {
    u32 i = 0, j = xpos;
    for (; j < 256; ++i, ++j) scrolled[i] = raw[j];
    for (; i < 256; ++i) scrolled[i] = 0;
  }
  return scrolled;
}

// ---- registers ----------------------------------------------------------------

u32 Gpu3D::read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  if ((r & ~3u) == 0x600) {
    // GXSTAT first, ahead of the width split and the switch: a game waiting
    // for a swap polls it tens of thousands of times a frame (Dragon Ball
    // Origins: ~25k/frame through its intro, 99.98 % of them with the engine
    // idle and a flush pending, so run_to() returns at once). Measured under
    // qemu the read is ~64 instructions either way: the cost is sched.now(),
    // the run_to test and composing the value, not the dispatch. The poll
    // itself is what the idle-loop skip (DS_IDLE_SKIP) removes.
    if (prof::enabled) {
      prof::add(prof::C_GX_READ, 1); prof::add(prof::C_GX_READ_GXSTAT, 1);
      if (gxstat_ & (1u << 27)) prof::add(prof::C_GX_READ_GXSTAT_BUSY, 1);
      if (pipe_n_) prof::add(prof::C_GX_READ_GXSTAT_PIPE, 1);
      if (fifo_n_) prof::add(prof::C_GX_READ_GXSTAT_FIFO, 1);
    }
    run_to(nds_.sched.now());
    const u32 level = fifo_n_;
    const u32 v = gxstat_ | ((pos_sp_ & 0x1F) << 8) | ((proj_sp_ & 1) << 13) | (level << 16) |
                  (level < 128 ? (1u << 25) : 0) | (level == 0 ? (1u << 26) : 0);
    return width == 32 ? v : width == 16 ? (v >> ((addr & 2) * 8)) & 0xFFFF : (v >> ((addr & 3) * 8)) & 0xFF;
  }
  if (width == 8) { const u32 v = read(addr & ~3u, 32); return (v >> ((addr & 3) * 8)) & 0xFF; }
  if (width == 16) { const u32 v = read(addr & ~3u, 32); return (v >> ((addr & 2) * 8)) & 0xFFFF; }
  prof::add(prof::C_GX_READ, 1);
  switch (r) {
  case 0x60: return dispcnt_;
  case 0x320: return 46;                         // RDLINES_COUNT: rendering keeps up
  case 0x604: return num_polygons_ | (num_vertices_ << 16);
  case 0x620: return static_cast<u32>(pos_test_[0]);
  case 0x624: return static_cast<u32>(pos_test_[1]);
  case 0x628: return static_cast<u32>(pos_test_[2]);
  case 0x62C: return static_cast<u32>(pos_test_[3]);
  case 0x630: return static_cast<u16>(vec_test_[0]) | (static_cast<u32>(static_cast<u16>(vec_test_[1])) << 16);
  case 0x634: return static_cast<u16>(vec_test_[2]);
  case 0x680: return static_cast<u32>(vec_[0]); case 0x684: return static_cast<u32>(vec_[1]); case 0x688: return static_cast<u32>(vec_[2]);
  case 0x68C: return static_cast<u32>(vec_[4]); case 0x690: return static_cast<u32>(vec_[5]); case 0x694: return static_cast<u32>(vec_[6]);
  case 0x698: return static_cast<u32>(vec_[8]); case 0x69C: return static_cast<u32>(vec_[9]); case 0x6A0: return static_cast<u32>(vec_[10]);
  default: break;
  }
  if (r >= 0x640 && r < 0x680) { update_clip_matrix(); return static_cast<u32>(clip_[(r & 0x3C) >> 2]); }
  return 0;
}

void Gpu3D::write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // The command ports first, ahead of the width split and the register
  // switches: a 3D frame is tens of thousands of 32-bit writes to GXFIFO or
  // the direct command ports and a handful to anything else (Dragon Ball
  // Origins: ~22k a frame, most of them by DMA -- see Dma::run_channel).
  if (width == 32 && r - 0x400 < 0x1CC) {
    if (!geometry_on_) return;
    if (r < 0x440) gxfifo_write(value);
    else fifo_write(Entry{value, static_cast<u8>((r & 0x1FC) >> 2)});
    return;
  }
  if (!rendering_on_ && r >= 0x320 && r < 0x400) return;
  if (!geometry_on_ && r >= 0x400 && r < 0x700) return;

  if (width == 8) {
    switch (r) {
    case 0x60:
      dispcnt_ = (dispcnt_ & 0xFF00) | (value & 0xFF);
      alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
      return;
    case 0x61:   // bits 12/13 are sticky error flags, cleared by writing 1
      dispcnt_ = (dispcnt_ & 0x30FF) | ((value & 0x4F) << 8);
      if (value & 0x10) dispcnt_ &= ~(1u << 12);
      if (value & 0x20) dispcnt_ &= ~(1u << 13);
      return;
    case 0x62: case 0x63: return;
    case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
    case 0x601: if (value & 0x80) { gxstat_ &= ~0x8000u; proj_sp_ = 0; tex_sp_ = 0; } return;
    case 0x603: gxstat_ = (gxstat_ & 0x3FFFFFFF) | ((value & 0xC0) << 24); check_fifo_irq(); return;
    default: break;
    }
    if (r >= 0x330 && r < 0x340) { const u32 i = (r - 0x330) >> 1; edge_[i] = (r & 1) ? static_cast<u16>((edge_[i] & 0x00FF) | (value << 8)) : static_cast<u16>((edge_[i] & 0xFF00) | (value & 0xFF)); return; }
    if (r >= 0x360 && r < 0x380) { fog_density_[r - 0x360] = value & 0x7F; return; }
    if (r >= 0x380 && r < 0x3C0) { const u32 i = (r - 0x380) >> 1; toon_[i] = (r & 1) ? static_cast<u16>((toon_[i] & 0x00FF) | (value << 8)) : static_cast<u16>((toon_[i] & 0xFF00) | (value & 0xFF)); return; }
    if (r >= 0x350 && r < 0x360) {               // clear attributes / fog colour / fog offset bytes
      const u32 cur = read(addr & ~1u, 16);
      write(addr & ~1u, 16, (addr & 1) ? ((cur & 0x00FF) | ((value & 0xFF) << 8)) : ((cur & 0xFF00) | (value & 0xFF)));
      return;
    }
    return;
  }

  if (width == 16) {
    switch (r) {
    case 0x60:
      dispcnt_ = (value & 0x4FFF) | (dispcnt_ & 0x3000);
      if (value & (1 << 12)) dispcnt_ &= ~(1u << 12);
      if (value & (1 << 13)) dispcnt_ &= ~(1u << 13);
      alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
      return;
    case 0x62: return;
    case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
    case 0x350: clear_attr1_ = (clear_attr1_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x352: clear_attr1_ = (clear_attr1_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x354: clear_attr2_ = (clear_attr2_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x356: clear_attr2_ = (clear_attr2_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x358: fog_color_ = (fog_color_ & 0xFFFF0000) | (value & 0xFFFF); return;
    case 0x35A: fog_color_ = (fog_color_ & 0x0000FFFF) | ((value & 0xFFFF) << 16); return;
    case 0x35C: fog_offset_ = value & 0x7FFF; return;
    case 0x600: if (value & 0x8000) { gxstat_ &= ~0x8000u; proj_sp_ = 0; tex_sp_ = 0; } return;
    case 0x602: gxstat_ = (gxstat_ & 0x3FFFFFFF) | ((value & 0xC000) << 16); check_fifo_irq(); return;
    case 0x610: zero_dot_w_limit_ = ((value & 0x7FFF) * 0x200) + 0x1FF; return;
    default: break;
    }
    if (r >= 0x330 && r < 0x340) { edge_[(r - 0x330) >> 1] = static_cast<u16>(value); return; }
    if (r >= 0x360 && r < 0x380) { fog_density_[r - 0x360] = value & 0x7F; fog_density_[r - 0x360 + 1] = (value >> 8) & 0x7F; return; }
    if (r >= 0x380 && r < 0x3C0) { toon_[(r - 0x380) >> 1] = static_cast<u16>(value); return; }
    return;
  }

  switch (r) {
  case 0x60:
    dispcnt_ = (value & 0x4FFF) | (dispcnt_ & 0x3000);
    if (value & (1 << 12)) dispcnt_ &= ~(1u << 12);
    if (value & (1 << 13)) dispcnt_ &= ~(1u << 13);
    alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0;
    return;
  case 0x340: alpha_ref_val_ = value & 0x1F; alpha_ref_ = (dispcnt_ & 4) ? alpha_ref_val_ : 0; return;
  case 0x350: clear_attr1_ = value; return;
  case 0x354: clear_attr2_ = value; return;
  case 0x358: fog_color_ = value; return;
  case 0x35C: fog_offset_ = value & 0x7FFF; return;
  case 0x600:
    if (value & 0x8000) { gxstat_ &= ~0x8000u; proj_sp_ = 0; tex_sp_ = 0; }
    gxstat_ = (gxstat_ & 0x3FFFFFFF) | (value & 0xC0000000);
    check_fifo_irq();
    return;
  case 0x610: zero_dot_w_limit_ = ((value & 0x7FFF) * 0x200) + 0x1FF; return;
  default: break;
  }
  if (r >= 0x400 && r < 0x440) { gxfifo_write(value); return; }
  if (r >= 0x440 && r < 0x5CC) { fifo_write(Entry{value, static_cast<u8>((r & 0x1FC) >> 2)}); return; }
  if (r >= 0x330 && r < 0x340) { const u32 i = (r - 0x330) >> 1; edge_[i] = value & 0xFFFF; edge_[i + 1] = value >> 16; return; }
  if (r >= 0x360 && r < 0x380) { const u32 i = r - 0x360; for (int k = 0; k < 4; ++k) fog_density_[i + k] = (value >> (8 * k)) & 0x7F; return; }
  if (r >= 0x380 && r < 0x3C0) { const u32 i = (r - 0x380) >> 1; toon_[i] = value & 0xFFFF; toon_[i + 1] = value >> 16; return; }
}

} // namespace ds::gpu
