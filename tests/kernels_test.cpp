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
    for (u32 i = 0; i < 256; ++i) { if (rng() & 1) line3d[i] &= 0x00FFFFFF; if (top_alpha[i] > 16 && (rng() & 1)) top_alpha[i] &= 0xF; }
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
  }
}

int main() {
  test_select();
  test_composite();
  test_palette_and_tiles();
  test_output();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
#if DSPERATE_NEON
  std::puts("kernels: ok (neon vs ref)");
#else
  std::puts("kernels: ok (ref only)");
#endif
  return 0;
}
