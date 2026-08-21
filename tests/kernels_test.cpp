// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Kernel twins: the NEON kernels must produce exactly what the portable
// reference produces, on random planes covering every branch. On hosts
// without NEON only the reference runs (as a smoke test).
#include "core/gpu/kernels.h"

#include <cstdio>
#include <cstring>
#include <random>

using namespace ds;
using namespace ds::gpu;

static int failures = 0;
static std::mt19937 rng(12345);

template <typename T> static void fill(T* p, u32 n, u32 mask) { for (u32 i = 0; i < n; ++i) p[i] = static_cast<T>(rng() & mask); }

struct Planes {
  alignas(16) Pixel px[256], top[256], second[256], out[256], col[256];
  alignas(16) u8 op[256], win[256], top_id[256], top_kind[256], top_alpha[256], second_id[256], attr[256], alpha[256];
  alignas(16) u32 line3d[256], dst[256];
  void randomise() {
    fill(px, 256, 0x1F3F3F3F); fill(top, 256, 0xFF3F3F3F); fill(second, 256, 0xFF3F3F3F); fill(col, 256, 0x3F3F3F);
    fill(op, 256, 1); fill(win, 256, 0xFF); fill(top_id, 256, 0x3F); fill(top_kind, 256, 3); fill(top_alpha, 256, 0x1F); fill(second_id, 256, 0x3F);
    fill(attr, 256, 0xFF); fill(alpha, 256, 0x1F); fill(line3d, 256, 0x1F3F3F3F); fill(dst, 256, 0x3F3F3F);
    // OBJ alphas are EVA values (bitmap alpha + 1, at most 16); 3D alphas live in the top record.
    for (u32 i = 0; i < 256; ++i) { if (rng() & 1) line3d[i] &= 0x00FFFFFF; top_alpha[i] %= 17; }
    for (u32 i = 0; i < 256; ++i) { if (!(rng() & 3)) top_kind[i] = K_NORMAL; }
    // ids are one-hot on real lines.
    for (u32 i = 0; i < 256; ++i) { top_id[i] = 1 << (rng() % 6); second_id[i] = 1 << (rng() % 6); }
  }
};

#define CHECK_SAME(name, a, b, bytes) do { if (std::memcmp(a, b, bytes)) { std::fprintf(stderr, "FAIL %s: %s differs (iteration %u)\n", __func__, name, it); ++failures; } } while (0)

#if DSPERATE_NEON
namespace N = kern::neon;
#else
namespace N = kern::ref;
#endif

static void test_select() {
  for (u32 it = 0; it < 200; ++it) {
    Planes a; a.randomise(); Planes b = a;
    const u8 wbit = 1 << (rng() % 5), id = 1 << (rng() % 5); const bool is3d = rng() & 1;
    kern::ref::select_plane(a.px, a.op, a.win, wbit, id, is3d, a.top, a.second, a.top_id, a.top_kind, a.top_alpha, a.second_id);
    N::select_plane(b.px, b.op, b.win, wbit, id, is3d, b.top, b.second, b.top_id, b.top_kind, b.top_alpha, b.second_id);
    CHECK_SAME("top", a.top, b.top, sizeof a.top); CHECK_SAME("second", a.second, b.second, sizeof a.second);
    CHECK_SAME("top_id", a.top_id, b.top_id, 256); CHECK_SAME("top_kind", a.top_kind, b.top_kind, 256);
    CHECK_SAME("top_alpha", a.top_alpha, b.top_alpha, 256); CHECK_SAME("second_id", a.second_id, b.second_id, 256);
    const u32 prio = rng() & 3;
    kern::ref::select_obj(a.col, a.attr, a.alpha, a.win, prio, a.top, a.second, a.top_id, a.top_kind, a.top_alpha, a.second_id);
    N::select_obj(b.col, b.attr, b.alpha, b.win, prio, b.top, b.second, b.top_id, b.top_kind, b.top_alpha, b.second_id);
    CHECK_SAME("obj top", a.top, b.top, sizeof a.top); CHECK_SAME("obj second", a.second, b.second, sizeof a.second);
    CHECK_SAME("obj top_id", a.top_id, b.top_id, 256); CHECK_SAME("obj top_kind", a.top_kind, b.top_kind, 256);
    CHECK_SAME("obj top_alpha", a.top_alpha, b.top_alpha, 256); CHECK_SAME("obj second_id", a.second_id, b.second_id, 256);
  }
}

static void test_composite() {
  for (u32 it = 0; it < 400; ++it) {
    Planes a; a.randomise(); Planes b = a;
    const u32 bldcnt = rng() & 0x3FFF, eva = rng() % 17, evb = rng() % 17, evy = rng() % 17;
    kern::ref::composite_line(bldcnt, eva, evb, evy, a.top, a.second, a.top_id, a.top_kind, a.top_alpha, a.second_id, a.win, a.out);
    N::composite_line(bldcnt, eva, evb, evy, b.top, b.second, b.top_id, b.top_kind, b.top_alpha, b.second_id, b.win, b.out);
    CHECK_SAME("out", a.out, b.out, sizeof a.out);
  }
}

static void test_palette_and_tiles() {
  alignas(16) u16 pal[512]; alignas(16) Pixel p18a[512], p18b[512];
  alignas(16) u8 idx[8]; alignas(16) Pixel pxa[8], pxb[8]; alignas(16) u8 opa[8], opb[8];
  for (u32 it = 0; it < 100; ++it) {
    fill(pal, 512, 0xFFFF);
    kern::ref::palette_to_18(pal, p18a, 512); N::palette_to_18(pal, p18b, 512);
    CHECK_SAME("pal18", p18a, p18b, sizeof p18a);
    fill(idx, 8, 0xF);
    kern::ref::tile_row_pal16(idx, p18a + (it & 15) * 16, pxa, opa); N::tile_row_pal16(idx, p18a + (it & 15) * 16, pxb, opb);
    CHECK_SAME("tile px", pxa, pxb, sizeof pxa); CHECK_SAME("tile op", opa, opb, sizeof opa);
  }
}

// Whole-row text kernels: random packed/8-bit tile rows, palette numbers and
// flips, including rows that are entirely transparent (the `any` result).
static void test_text_tiles() {
  alignas(16) u16 pal[4096]; alignas(16) Pixel p18[4096];
  alignas(16) u8 packed[33 * 4], rows[33 * 8], ctl[33];
  alignas(16) Pixel pxa[33 * 8 + 8], pxb[33 * 8 + 8]; alignas(16) u8 opa[33 * 8 + 16], opb[33 * 8 + 16];
  fill(pal, 4096, 0xFFFF); kern::ref::palette_to_18(pal, p18, 4096);
  const Pixel* pals[16]; for (u32 i = 0; i < 16; ++i) pals[i] = p18 + i * 256;
  for (u32 it = 0; it < 300; ++it) {
    const u32 n = 1 + rng() % 33, mask = (it & 7) == 0 ? 0 : 0xFF;
    fill(packed, 33 * 4, mask); fill(rows, 33 * 8, mask); fill(ctl, 33, 0x1F);
    const u32 shift = rng() & 7;   // unaligned destination, as the engine uses it
    const bool a16 = kern::ref::text_tiles_16(packed, ctl, p18, n, pxa + shift, opa + shift);
    const bool b16 = N::text_tiles_16(packed, ctl, p18, n, pxb + shift, opb + shift);
    if (a16 != b16) { std::fprintf(stderr, "FAIL text_tiles_16 any (iteration %u)\n", it); ++failures; }
    CHECK_SAME("text16 px", pxa + shift, pxb + shift, n * 32); CHECK_SAME("text16 op", opa + shift, opb + shift, n * 8);
    const bool a256 = kern::ref::text_tiles_256(rows, ctl, pals, n, pxa + shift, opa + shift);
    const bool b256 = N::text_tiles_256(rows, ctl, pals, n, pxb + shift, opb + shift);
    if (a256 != b256) { std::fprintf(stderr, "FAIL text_tiles_256 any (iteration %u)\n", it); ++failures; }
    CHECK_SAME("text256 px", pxa + shift, pxb + shift, n * 32); CHECK_SAME("text256 op", opa + shift, opb + shift, n * 8);
  }
}

static void test_output() {
  for (u32 it = 0; it < 200; ++it) {
    Planes a; a.randomise(); Planes b = a;
    kern::ref::layer_3d(a.line3d, a.px, a.op); N::layer_3d(b.line3d, b.px, b.op);
    CHECK_SAME("3d px", a.px, b.px, sizeof a.px); CHECK_SAME("3d op", a.op, b.op, 256);
    const u16 reg = static_cast<u16>(((rng() % 3) << 14) | (rng() & 0x1F));
    kern::ref::master_brightness(reg, a.dst); N::master_brightness(reg, b.dst);
    CHECK_SAME("brightness", a.dst, b.dst, sizeof a.dst);
    kern::ref::expand_colours(a.dst); N::expand_colours(b.dst);
    CHECK_SAME("expand", a.dst, b.dst, sizeof a.dst);
    kern::ref::output_line(a.top, reg, a.dst); N::output_line(b.top, reg, b.dst);
    CHECK_SAME("output_line", a.dst, b.dst, sizeof a.dst);
  }
}

// 3D span stages: random spans and endpoints; every branch of the reference
// (ascending / descending / equal attributes, all span widths, numerator wrap).
static void test_span() {
  alignas(16) u32 fa[256], fb[256]; alignas(16) s32 oa[256], ob[256];
  for (u32 it = 0; it < 2000; ++it) {
    const s32 xdiff = 1 + static_cast<s32>(rng() % 257);
    const s32 xv0 = static_cast<s32>(rng() % static_cast<u32>(xdiff));
    const u32 n = 1 + rng() % static_cast<u32>(xdiff - xv0);
    s32 w0 = static_cast<s32>(rng() & 0xFFFF), w1 = static_cast<s32>(rng() & 0xFFFF);
    if (!(rng() & 3)) w1 = w0;                       // equal W
    if (!(rng() & 7)) w0 = 0;                        // degenerate
    if (!(rng() & 15)) { w0 = static_cast<s32>(rng()); w1 = static_cast<s32>(rng()); }   // garbage W: wrap paths
    kern::ref::span_factor(xv0, n, xdiff, w0, w0, w1, fa);
    N::span_factor(xv0, n, xdiff, w0, w0, w1, fb);
    CHECK_SAME("span_factor", fa, fb, n * 4);
    const u32 kind = rng() % 3;
    s32 y0, y1;
    if (kind == 0) { y0 = static_cast<s32>(rng() & 0x1FF); y1 = static_cast<s32>(rng() & 0x1FF); }                  // colour
    else if (kind == 1) { y0 = static_cast<s16>(rng()); y1 = static_cast<s16>(rng()); }                               // texture coordinate
    else { y0 = static_cast<s32>(rng() & 0xFFFFFF); y1 = static_cast<s32>(rng() & 0xFFFFFF); }                       // depth
    if (!(rng() & 7)) y1 = y0;
    kern::ref::span_attr_persp(y0, y1, fa, n, oa); N::span_attr_persp(y0, y1, fa, n, ob);
    CHECK_SAME("span_attr_persp", oa, ob, n * 4);
    kern::ref::span_attr_linear(y0, y1, xv0, n, xdiff, oa); N::span_attr_linear(y0, y1, xv0, n, xdiff, ob);
    CHECK_SAME("span_attr_linear", oa, ob, n * 4);
    const s32 xrecip = (1 << 22) / xdiff;
    kern::ref::span_z_linear(y0, y1, xv0, n, xdiff, xrecip, oa); N::span_z_linear(y0, y1, xv0, n, xdiff, xrecip, ob);
    CHECK_SAME("span_z_linear", oa, ob, n * 4);
  }
}

// Sprite row plot: random plane state and rows, every priority pairing, odd lengths.
static void test_obj_row() {
  alignas(16) u8 idx[80]; alignas(16) u16 col[80];
  alignas(16) u32 pxa[80], pxb[80]; alignas(16) u8 aa[80], ab[80], ala[80], alb[80];
  for (u32 it = 0; it < 500; ++it) {
    fill(idx, 80, (it & 3) == 0 ? 0x1 : 0xFF); fill(col, 80, 0xFFFF);
    fill(pxa, 80, 0xFFFF); fill(aa, 80, 0xBF); fill(ala, 80, 0x1F);
    for (u32 i = 0; i < 80; ++i) if (rng() & 1) aa[i] &= ~OA_OPAQUE;
    std::memcpy(pxb, pxa, sizeof pxa); std::memcpy(ab, aa, 80); std::memcpy(alb, ala, 80);
    const u32 n = 1 + rng() % 64, off = rng() % 16;
    const u8 attr = static_cast<u8>(rng() & 0x3F), alpha = static_cast<u8>(1 + rng() % 16);
    const u32 pal_base = (rng() & 1) ? (OP_STDPAL | ((rng() & 0xF) << 4)) : ((rng() & 0xF) << 8);
    if (rng() & 1) { kern::ref::obj_row_idx(idx, n, pal_base, attr, pxa + off, aa + off, ala + off); N::obj_row_idx(idx, n, pal_base, attr, pxb + off, ab + off, alb + off); }
    else { kern::ref::obj_row_bmp(col, n, attr, alpha, pxa + off, aa + off, ala + off); N::obj_row_bmp(col, n, attr, alpha, pxb + off, ab + off, alb + off); }
    CHECK_SAME("obj px", pxa, pxb, sizeof pxa); CHECK_SAME("obj attr", aa, ab, 80); CHECK_SAME("obj alpha", ala, alb, 80);
  }
}

int main() {
  test_obj_row();
  test_select();
  test_composite();
  test_palette_and_tiles();
  test_text_tiles();
  test_output();
  test_span();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
#if DSPERATE_NEON
  std::puts("kernels: ok (neon vs ref)");
#else
  std::puts("kernels: ok (ref only)");
#endif
  return 0;
}
