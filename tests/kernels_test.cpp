// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Kernel twins: the NEON kernels must produce exactly what the portable
// reference produces, on random planes covering every branch. On hosts
// without NEON only the reference runs (as a smoke test).
#include "core/gpu/kernels.h"

#include <cstdio>
#include <cstring>
#include <vector>
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


static void test_select16() {
  alignas(16) u16 v[256], ov[256], ta[256], tb[256], sa[256], sb[256];
  alignas(16) u8 win[256], attr[256], tta[256], ttb[256], sta[256], stb[256];
  for (u32 it = 0; it < 300; ++it) {
    for (u32 i = 0; i < 256; ++i) {
      v[i] = static_cast<u16>(rng()); ov[i] = static_cast<u16>(rng()); win[i] = static_cast<u8>(rng());
      attr[i] = static_cast<u8>(rng()); ta[i] = tb[i] = static_cast<u16>(rng()); sa[i] = sb[i] = static_cast<u16>(rng());
      tta[i] = ttb[i] = rng() & 7; sta[i] = stb[i] = rng() & 7;
    }
    const u8 wbit = 1 << (rng() % 5), tid = rng() & 3; const u32 prio = rng() & 3;
    kern::ref::select16(v, win, wbit, tid, ta, tta, sa, sta); N::select16(v, win, wbit, tid, tb, ttb, sb, stb);
    CHECK_SAME("s16 top", ta, tb, sizeof ta); CHECK_SAME("s16 tid", tta, ttb, 256); CHECK_SAME("s16 second", sa, sb, sizeof sa); CHECK_SAME("s16 stid", sta, stb, 256);
    kern::ref::select16_obj(ov, attr, win, prio, ta, tta, sa, sta); N::select16_obj(ov, attr, win, prio, tb, ttb, sb, stb);
    CHECK_SAME("o16 top", ta, tb, sizeof ta); CHECK_SAME("o16 tid", tta, ttb, 256); CHECK_SAME("o16 second", sa, sb, sizeof sa); CHECK_SAME("o16 stid", sta, stb, 256);
    kern::ref::select16_flat(v, win, wbit, tid, ta, tta); N::select16_flat(v, win, wbit, tid, tb, ttb);
    CHECK_SAME("f16 top", ta, tb, sizeof ta); CHECK_SAME("f16 tid", tta, ttb, 256);
    kern::ref::select16_obj_flat(ov, attr, win, prio, ta, tta); N::select16_obj_flat(ov, attr, win, prio, tb, ttb);
    CHECK_SAME("of16 top", ta, tb, sizeof ta); CHECK_SAME("of16 tid", tta, ttb, 256);
  }
}

static void test_resolve16() {
  static Pixel tabs[8][32768];
  const Pixel* tables[8];
  for (u32 t = 0; t < 8; ++t) { for (u32 i = 0; i < 32768; ++i) tabs[t][i] = rng() & 0x3F3F3F; tables[t] = tabs[t]; }
  alignas(16) u16 top[256]; alignas(16) u8 tid[256]; alignas(16) Pixel oa[256], ob[256];
  for (u32 it = 0; it < 200; ++it) {
    const bool uniform = it & 1;
    for (u32 i = 0; i < 256; ++i) { top[i] = static_cast<u16>(rng()); tid[i] = uniform ? (it >> 1) & 7 : rng() & 7; }
    kern::ref::resolve16(top, tid, tables, oa); N::resolve16(top, tid, tables, ob);
    CHECK_SAME("resolve16", oa, ob, sizeof oa);
  }
}

static void test_resolve16_one() {
  static Pixel tab[32768];
  for (u32 i = 0; i < 32768; ++i) tab[i] = rng() & 0x3F3F3F;
  alignas(16) u16 v[256]; alignas(16) Pixel oa[256], ob[256];
  for (u32 it = 0; it < 200; ++it) {
    for (u32 i = 0; i < 256; ++i) v[i] = static_cast<u16>(rng());
    kern::ref::resolve16_one(v, tab, oa); N::resolve16_one(v, tab, ob);
    CHECK_SAME("resolve16_one", oa, ob, sizeof oa);
  }
}

static void test_resolve16_full() {
  static Pixel tabs[9][32768];
  const Pixel* tables[9];
  for (u32 t = 0; t < 9; ++t) { for (u32 i = 0; i < 32768; ++i) tabs[t][i] = rng() & 0x3F3F3F; tables[t] = tabs[t]; }
  alignas(16) u16 top[256], second[256]; alignas(16) u8 tt[256], st[256], attr[256], alpha[256]; alignas(16) Pixel line3d[256];
  alignas(16) Pixel tpa[256], tpb[256], spa[256], spb[256]; alignas(16) u8 ia[256], ib[256], ka[256], kb[256], aa[256], ab[256], sa[256], sb[256];
  for (u32 it = 0; it < 200; ++it) {
    for (u32 i = 0; i < 256; ++i) { top[i] = static_cast<u16>(rng()); second[i] = static_cast<u16>(rng()); tt[i] = rng() % 9; st[i] = rng() % 9; attr[i] = static_cast<u8>(rng()); alpha[i] = rng() % 17; line3d[i] = rng() & 0x1FFFFFFF; }
    const Pixel* l3 = (it & 1) ? line3d : nullptr;
    kern::ref::resolve16_full(top, tt, second, st, tables, attr, alpha, l3, tpa, spa, ia, ka, aa, sa);
    N::resolve16_full(top, tt, second, st, tables, attr, alpha, l3, tpb, spb, ib, kb, ab, sb);
    CHECK_SAME("rf top", tpa, tpb, sizeof tpa); CHECK_SAME("rf second", spa, spb, sizeof spa); CHECK_SAME("rf id", ia, ib, 256);
    CHECK_SAME("rf kind", ka, kb, 256); CHECK_SAME("rf alpha", aa, ab, 256); CHECK_SAME("rf sid", sa, sb, 256);
  }
}

static void test_resolve16_top() {
  static Pixel tabs[9][32768];
  const Pixel* tables[9];
  for (u32 t = 0; t < 9; ++t) { for (u32 i = 0; i < 32768; ++i) tabs[t][i] = rng() & 0x3F3F3F; tables[t] = tabs[t]; }
  alignas(16) u16 top[256]; alignas(16) u8 tt[256]; alignas(16) Pixel line3d[256];
  alignas(16) Pixel tpa[256], tpb[256]; alignas(16) u8 ia[256], ib[256];
  for (u32 it = 0; it < 200; ++it) {
    // Every third iteration is one layer for the whole block, the run the
    // NEON path hoists the table pointer for.
    const bool run = it % 3 == 0;
    const u8 one = static_cast<u8>(rng() % 9);
    for (u32 i = 0; i < 256; ++i) { top[i] = static_cast<u16>(rng()); tt[i] = run ? one : static_cast<u8>(rng() % 9); line3d[i] = rng() & 0x1FFFFFFF; }
    const Pixel* l3 = (it & 1) ? line3d : nullptr;
    kern::ref::resolve16_top(top, tt, tables, l3, tpa, ia);
    N::resolve16_top(top, tt, tables, l3, tpb, ib);
    CHECK_SAME("rt top", tpa, tpb, sizeof tpa); CHECK_SAME("rt id", ia, ib, 256);
  }
}

static void test_composite_fade() {
  for (u32 it = 0; it < 400; ++it) {
    Planes a; a.randomise(); Planes b = a;
    // Only the fade effects reach this kernel (see Engine2D::needs_second).
    const u32 bldcnt = (rng() & 0x3FFF & ~0x00C0u) | ((2 + (rng() & 1)) << 6), evy = rng() % 17;
    kern::ref::composite_line_fade(bldcnt, evy, a.top, a.top_id, a.win, a.out);
    N::composite_line_fade(bldcnt, evy, b.top, b.top_id, b.win, b.out);
    CHECK_SAME("fade out", a.out, b.out, sizeof a.out);
  }
}

static void test_rows16() {
  alignas(16) u8 packed[33 * 4], rows[33 * 8], ctl[33]; alignas(16) u16 va[33 * 8], vb[33 * 8];
  for (u32 it = 0; it < 200; ++it) {
    fill(packed, sizeof packed, 0xFF); fill(rows, sizeof rows, 0xFF); fill(ctl, 33, 0x1F);
    if (it % 5 == 0) { std::memset(packed, 0, sizeof packed); std::memset(rows, 0, sizeof rows); }
    const u32 n = 1 + rng() % 33;
    const bool ra = kern::ref::text_row_16(packed, ctl, n, va), rb = N::text_row_16(packed, ctl, n, vb);
    if (ra != rb) { std::fprintf(stderr, "FAIL text_row_16 any %d vs %d\n", ra, rb); ++failures; }
    CHECK_SAME("text_row_16", va, vb, n * 16);
    const bool ext = rng() & 1;
    const bool ra2 = kern::ref::text_row_256(rows, ctl, n, ext, va), rb2 = N::text_row_256(rows, ctl, n, ext, vb);
    if (ra2 != rb2) { std::fprintf(stderr, "FAIL text_row_256 any %d vs %d\n", ra2, rb2); ++failures; }
    CHECK_SAME("text_row_256", va, vb, n * 16);
  }
  alignas(16) u32 line[256]; alignas(16) u16 la[256], lb[256];
  for (u32 it = 0; it < 100; ++it) {
    for (auto& x : line) x = rng() & (rng() & 1 ? 0x1FFFFFFF : 0x00FFFFFF);
    kern::ref::layer16_3d(line, la); N::layer16_3d(line, lb);
    CHECK_SAME("layer16_3d", la, lb, sizeof la);
  }
}

static void test_obj_row16() {
  alignas(16) u8 idx[80]; alignas(16) u16 col[80];
  alignas(16) u16 pa[80], pb[80]; alignas(16) u8 aa[80], ab[80], la[80], lb[80];
  for (u32 it = 0; it < 300; ++it) {
    fill(idx, 80, 0xFF); for (auto& c : col) c = static_cast<u16>(rng());
    for (u32 i = 0; i < 80; ++i) { pa[i] = pb[i] = static_cast<u16>(rng()); aa[i] = ab[i] = static_cast<u8>(rng()); la[i] = lb[i] = static_cast<u8>(rng()); }
    const u32 n = 1 + rng() % 64; const u8 attr = static_cast<u8>(rng() & 0x7F), alpha = rng() % 17; const u16 pal_base = static_cast<u16>((rng() & 0xF) << (rng() & 1 ? 4 : 8));
    if (rng() & 1) { kern::ref::obj_row_idx16(idx, n, pal_base, attr, pa, aa, la); N::obj_row_idx16(idx, n, pal_base, attr, pb, ab, lb); }
    else { kern::ref::obj_row_bmp16(col, n, attr, alpha, pa, aa, la); N::obj_row_bmp16(col, n, attr, alpha, pb, ab, lb); }
    CHECK_SAME("obj16 v", pa, pb, sizeof pa); CHECK_SAME("obj16 attr", aa, ab, 80); CHECK_SAME("obj16 alpha", la, lb, 80);
  }
}

static void test_palette() {
  alignas(16) u16 pal[512]; alignas(16) Pixel p18a[512], p18b[512];
  for (u32 it = 0; it < 100; ++it) {
    fill(pal, 512, 0xFFFF);
    kern::ref::palette_to_18(pal, p18a, 512); N::palette_to_18(pal, p18b, 512);
    CHECK_SAME("pal18", p18a, p18b, sizeof p18a);
  }
}

static void test_translucent_3d() {
  alignas(16) Pixel line[256];
  for (u32 it = 0; it < 300; ++it) {
    for (auto& v : line) { const u32 a = (it % 3 == 0) ? (rng() & 1 ? 31 : 0) : (rng() % 32); v = (rng() & 0xFFFFFF) | (a << 24); }
    const bool r = kern::ref::line_has_translucent_3d(line), n = N::line_has_translucent_3d(line);
    if (r != n) { std::fprintf(stderr, "FAIL line_has_translucent_3d %d vs %d (iteration %u)\n", r, n, it); ++failures; }
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


static void test_output() {
  for (u32 it = 0; it < 200; ++it) {
    Planes a; a.randomise(); Planes b = a;
    const u16 reg = static_cast<u16>(((rng() % 3) << 14) | (rng() & 0x1F));
    kern::ref::master_brightness(reg, a.dst); N::master_brightness(reg, b.dst);
    CHECK_SAME("brightness", a.dst, b.dst, sizeof a.dst);
    kern::ref::expand_colours(a.dst); N::expand_colours(b.dst);
    CHECK_SAME("expand", a.dst, b.dst, sizeof a.dst);
    kern::ref::output_line(a.top, reg, a.dst); N::output_line(b.top, reg, b.dst);
    CHECK_SAME("output_line", a.dst, b.dst, sizeof a.dst);
  }
}

static void test_capture() {
  alignas(16) Pixel srca[256]; alignas(16) u16 srcb[256], da[256], db[256];
  for (u32 it = 0; it < 300; ++it) {
    // Records as the engines emit them (6-bit channels, alpha 0/0xFF or the
    // 3D line's 5-bit alpha), and every third round fully random bytes.
    for (auto& v : srca) {
      if (it % 3 == 2) v = rng();
      else v = (rng() & 0x3F3F3F) | ((it % 3 ? (rng() % 32) : (rng() & 1 ? 0xFFu : 0u)) << 24);
    }
    for (auto& v : srcb) v = static_cast<u16>(rng());
    const u32 n = rng() & 1 ? 256 : 128;
    kern::ref::capture_a15(srca, n, da); N::capture_a15(srca, n, db);
    CHECK_SAME("capture_a15", da, db, n * sizeof(u16));
    const u32 eva = rng() % 17, evb = rng() % 17;
    kern::ref::capture_blend(srca, srcb, n, eva, evb, da); N::capture_blend(srca, srcb, n, eva, evb, db);
    CHECK_SAME("capture_blend", da, db, n * sizeof(u16));
  }
}

// scale_row at the destination widths that matter (the two boards' panels and
// the window sizes), plus the degenerate ones: 1:1, downscale, and a width
// that is not a multiple of the vector step.
static void test_scale_row() {
  static const u32 widths[] = {1, 100, 255, 256, 257, 320, 512, 640, 720, 721, 1280, 1440};
  alignas(16) u32 src[256];
  for (u32 w : widths) {
    const u32 it = w;                          // CHECK_SAME reports it as the iteration
    std::vector<u16> xrun(257);
    for (u32 i = 0; i <= 256; ++i) xrun[i] = static_cast<u16>((i * w + 255) / 256);
    std::vector<u32> da(w, 0xDEADBEEF), db(w, 0xDEADBEEF);
    for (u32& v : src) v = rng();
    kern::ref::scale_row(src, xrun.data(), da.data());
    N::scale_row(src, xrun.data(), db.data());
    CHECK_SAME("scale_row", da.data(), db.data(), w * sizeof(u32));
    // Every destination pixel written, and written with the source pixel the
    // frontend's dst_x -> src_x map picks -- the two must agree or the
    // scanline path lands pixels somewhere draw() would not.
    for (u32 x = 0; x < w; ++x)
      if (da[x] != src[x * 256 / w]) { std::printf("scale_row w=%u x=%u map mismatch\n", w, x); ++failures; break; }
  }
}

// scale_row_grid against the reference at the same widths, both row kinds and
// the factors that matter (0 = black seams, 128 = the default, 255 ~ off).
static void test_scale_row_grid() {
  static const u32 widths[] = {1, 100, 255, 256, 257, 320, 512, 640, 720, 721, 1280, 1440};
  alignas(16) u32 src[256];
  for (u32 w : widths) for (u32 f : {0u, 128u, 255u}) for (bool seam : {false, true}) for (u32 mr : {2u, (w + 255) / 256}) {
    const u32 it = w * 8 + f / 64 * 2 + seam;
    std::vector<u16> xrun(257);
    for (u32 i = 0; i <= 256; ++i) xrun[i] = static_cast<u16>((i * w + 255) / 256);
    std::vector<u32> da(w, 0xDEADBEEF), db(w, 0xDEADBEEF), plain(w);
    for (u32& v : src) v = rng();
    kern::ref::scale_row_grid(src, xrun.data(), f, mr, seam, da.data());
    N::scale_row_grid(src, xrun.data(), f, mr, seam, db.data());
    CHECK_SAME("scale_row_grid", da.data(), db.data(), w * sizeof(u32));
    kern::ref::scale_row(src, xrun.data(), plain.data());
    for (u32 x = 0; x < w; ++x) {                       // alpha kept; non-seam pixels untouched
      const u32 s_ = x * 256 / w;
      const bool dimmed = seam || (xrun[s_ + 1] - xrun[s_] >= std::max(2u, mr) && x == xrun[s_]);
      if ((!dimmed && da[x] != plain[x]) || (dimmed && f == 0 && da[x] != 0xFF000000u) || (dimmed && f && (da[x] >> 24) != (plain[x] >> 24))) {
        std::printf("scale_row_grid w=%u f=%u mr=%u seam=%d x=%u: %08x vs %08x\n", w, f, mr, seam, x, da[x], plain[x]); ++failures; break;
      }
    }
    (void)it;
  }
}

// scale_row_straddle / blend_line_w against the reference, and blend_line_w
// against its definition.
static void test_scale_row_straddle() {
  static const u32 widths[] = {1, 255, 256, 257, 640, 721, 1440};
  alignas(16) u32 src[256], seam[256];
  alignas(16) u8 w[256];
  for (u32 wd : widths) {
    const u32 it = wd;
    std::vector<u16> xrun(257);
    for (u32 i = 0; i <= 256; ++i) xrun[i] = static_cast<u16>((i * wd + 255) / 256);
    for (u32 s = 0; s < 256; ++s) { const u32 b = (s + 1) * wd, f = b % 256; w[s] = (s + 1 < 256 && f && xrun[s + 1] > xrun[s]) ? static_cast<u8>(f * 256 / 256) : 0; }
    std::vector<u32> da(wd, 0xDEADBEEF), db(wd, 0xDEADBEEF);
    for (u32& v : src) v = rng();
    for (u32& v : seam) v = rng();
    kern::ref::scale_row_straddle(src, seam, w, xrun.data(), da.data());
    N::scale_row_straddle(src, seam, w, xrun.data(), db.data());
    CHECK_SAME("scale_row_straddle", da.data(), db.data(), wd * sizeof(u32));
    for (u32 s = 0; s < 256; ++s) if (w[s] && xrun[s + 1] > xrun[s] && da[xrun[s + 1] - 1] != seam[s]) { std::printf("straddle w=%u s=%u\n", wd, s); ++failures; break; }
    alignas(16) u32 ba[256], bb[256];
    for (u8& v : w) v = static_cast<u8>(rng());
    kern::ref::blend_line_w(src, seam, w, ba);
    N::blend_line_w(src, seam, w, bb);
    CHECK_SAME("blend_line_w", ba, bb, sizeof ba);
    for (u32 i = 0; i < 256; ++i) for (u32 sh : {0u, 8u, 16u, 24u}) {
      const u32 xa = (src[i] >> sh) & 255, ya = (seam[i] >> sh) & 255, f = w[i];
      if (((ba[i] >> sh) & 255) != ((xa * (256 - f) + ya * f + 128) >> 8)) { std::printf("blend_line_w i=%u\n", i); ++failures; i = 256; break; }
    }
  }
}

// The vertical half of the same map, which lives in Gpu::emit_scaled: source
// line L owns destination rows [ceil(L*h/192), ceil((L+1)*h/192)). Every row
// of the destination must be claimed by exactly one source line, or the
// scanline path leaves gaps (or writes rows twice) where draw() would not.
static void test_scale_rows_cover() {
  static const u32 heights[] = {1, 96, 191, 192, 193, 240, 384, 480, 540, 541, 1080, 1200};
  for (u32 h : heights) {
    std::vector<u8> hits(h, 0);
    for (u32 line = 0; line < 192; ++line) {
      const u32 y0 = (line * h + 191) / 192, y1 = ((line + 1) * h + 191) / 192;
      for (u32 y = y0; y < y1; ++y) {
        if (y >= h) { std::printf("emit_scaled h=%u line=%u row %u out of range\n", h, line, y); ++failures; break; }
        ++hits[y];
      }
      // And the row each destination row would be sampled from must be the
      // one draw() picks for it.
      for (u32 y = y0; y < y1 && y < h; ++y)
        if (y * 192 / h != line) { std::printf("emit_scaled h=%u row=%u maps to %u not %u\n", h, y, y * 192 / h, line); ++failures; }
    }
    for (u32 y = 0; y < h; ++y)
      if (hits[y] != 1) { std::printf("emit_scaled h=%u row %u written %u times\n", h, y, hits[y]); ++failures; break; }
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
    {
      // The five-attribute pass: mixed flat / rising / falling endpoints.
      alignas(16) static s32 a5[5][264], b5[5][264];
      s32 ys0[5], ys1[5]; s32* pa[5]; s32* pb[5];
      for (int k = 0; k < 5; ++k) {
        ys0[k] = static_cast<s32>(rng() & 0xFFFFFF); ys1[k] = static_cast<s32>(rng() & 0xFFFFFF);
        if (!(rng() & 5)) ys1[k] = ys0[k];
        if (!(rng() & 3)) { const s32 t = ys0[k]; ys0[k] = ys1[k]; ys1[k] = t; }
        pa[k] = a5[k]; pb[k] = b5[k];
      }
      kern::ref::span_attrs5(ys0, ys1, fa, n, pa); N::span_attrs5(ys0, ys1, fa, n, pb);
      for (int k = 0; k < 5; ++k) CHECK_SAME("span_attrs5", a5[k], b5[k], n * 4);
      // The narrowing twin: colour to 6 bits, texture coordinates to s16.
      alignas(16) static u8 ra8[264], ga8[264], ba8[264], rb8[264], gb8[264], bb8[264];
      alignas(16) static s16 sa16[264], ta16[264], sb16[264], tb16[264];
      kern::ref::span_attrs5n(ys0, ys1, fa, n, ra8, ga8, ba8, sa16, ta16);
      N::span_attrs5n(ys0, ys1, fa, n, rb8, gb8, bb8, sb16, tb16);
      CHECK_SAME("span_attrs5n r", ra8, rb8, n);
      CHECK_SAME("span_attrs5n g", ga8, gb8, n);
      CHECK_SAME("span_attrs5n b", ba8, bb8, n);
      CHECK_SAME("span_attrs5n s", sa16, sb16, n * 2);
      CHECK_SAME("span_attrs5n t", ta16, tb16, n * 2);
      // The s/t-only twin, and its agreement with the five-attribute kernel
      // on the two attributes they share.
      kern::ref::span_attrs2n(ys0, ys1, fa, n, sa16, ta16);
      N::span_attrs2n(ys0, ys1, fa, n, sb16, tb16);
      CHECK_SAME("span_attrs2n s", sa16, sb16, n * 2);
      CHECK_SAME("span_attrs2n t", ta16, tb16, n * 2);
      kern::ref::span_attrs5n(ys0, ys1, fa, n, ra8, ga8, ba8, sb16, tb16);
      CHECK_SAME("span_attrs2n vs 5n s", sa16, sb16, n * 2);
      CHECK_SAME("span_attrs2n vs 5n t", ta16, tb16, n * 2);
    }
    if (kind != 2) {   // linear attributes: |y1 - y0| * xdiff < 2^32
      kern::ref::span_attr_linear(y0, y1, xv0, n, xdiff, oa); N::span_attr_linear(y0, y1, xv0, n, xdiff, ob);
      CHECK_SAME("span_attr_linear", oa, ob, n * 4);
      // The fused linear pass, against both its own reference and the
      // per-attribute kernel it replaces: five span_attr_linear calls plus
      // the narrowing store the caller used to do by hand. That equivalence
      // is the thing the renderer depends on, so check it directly.
      alignas(16) static s32 lin[5][264];
      alignas(16) static u8 lra[264], lga[264], lba[264], lrb[264], lgb[264], lbb[264], lrc[264], lgc[264], lbc[264];
      alignas(16) static s16 lsa[264], lta[264], lsb[264], ltb[264], lsc[264], ltc[264];
      s32 ls0[5], ls1[5];
      for (int k = 0; k < 5; ++k) {
        // Colour magnitudes for r/g/b, texture-coordinate ones for s/t.
        ls0[k] = k < 3 ? static_cast<s32>(rng() & 0x1FF) : static_cast<s16>(rng());
        ls1[k] = k < 3 ? static_cast<s32>(rng() & 0x1FF) : static_cast<s16>(rng());
        if (!(rng() & 5)) ls1[k] = ls0[k];
      }
      if (!(rng() & 3)) { ls1[0] = ls0[0]; ls1[1] = ls0[1]; ls1[2] = ls0[2]; }   // the flat-colour case
      kern::ref::span_attrs5n_lin(ls0, ls1, xv0, n, xdiff, lra, lga, lba, lsa, lta);
      N::span_attrs5n_lin(ls0, ls1, xv0, n, xdiff, lrb, lgb, lbb, lsb, ltb);
      CHECK_SAME("span_attrs5n_lin r", lra, lrb, n);
      CHECK_SAME("span_attrs5n_lin g", lga, lgb, n);
      CHECK_SAME("span_attrs5n_lin b", lba, lbb, n);
      CHECK_SAME("span_attrs5n_lin s", lsa, lsb, n * 2);
      CHECK_SAME("span_attrs5n_lin t", lta, ltb, n * 2);
      for (int k = 0; k < 5; ++k) kern::ref::span_attr_linear(ls0[k], ls1[k], xv0, n, xdiff, lin[k]);
      for (u32 i = 0; i < n; ++i) {
        lrc[i] = static_cast<u8>((static_cast<u32>(lin[0][i]) >> 3) & 0xFF);
        lgc[i] = static_cast<u8>((static_cast<u32>(lin[1][i]) >> 3) & 0xFF);
        lbc[i] = static_cast<u8>((static_cast<u32>(lin[2][i]) >> 3) & 0xFF);
        lsc[i] = static_cast<s16>(lin[3][i]);
        ltc[i] = static_cast<s16>(lin[4][i]);
      }
      CHECK_SAME("span_attrs5n_lin vs per-attribute r", lrc, lrb, n);
      CHECK_SAME("span_attrs5n_lin vs per-attribute g", lgc, lgb, n);
      CHECK_SAME("span_attrs5n_lin vs per-attribute b", lbc, lbb, n);
      CHECK_SAME("span_attrs5n_lin vs per-attribute s", lsc, lsb, n * 2);
      CHECK_SAME("span_attrs5n_lin vs per-attribute t", ltc, ltb, n * 2);
      kern::ref::span_attrs2n_lin(ls0, ls1, xv0, n, xdiff, lsa, lta);
      N::span_attrs2n_lin(ls0, ls1, xv0, n, xdiff, lsb, ltb);
      CHECK_SAME("span_attrs2n_lin s", lsa, lsb, n * 2);
      CHECK_SAME("span_attrs2n_lin t", lta, ltb, n * 2);
      CHECK_SAME("span_attrs2n_lin vs 5n_lin s", lsc, lsb, n * 2);
      CHECK_SAME("span_attrs2n_lin vs 5n_lin t", ltc, ltb, n * 2);
    }
    const s32 xrecip = (1 << 22) / xdiff;
    kern::ref::span_z_linear(y0, y1, xv0, n, xdiff, xrecip, oa); N::span_z_linear(y0, y1, xv0, n, xdiff, xrecip, ob);
    CHECK_SAME("span_z_linear", oa, ob, n * 4);
  }
}

// Sprite row plot: random plane state and rows, every priority pairing, odd lengths.

// Depth pre-pass: every mode, z values around the destination, edge flags.
static void test_depth_candidates() {
  alignas(16) s32 z[260]; alignas(16) u32 dz[260], da[260]; alignas(16) u8 pa[264], pb[264];
  for (u32 it = 0; it < 500; ++it) {
    const u32 n = 1 + rng() % 256;
    for (u32 i = 0; i < 260; ++i) {
      dz[i] = rng() & 0xFFFFFF; z[i] = static_cast<s32>(dz[i]) + static_cast<s32>(rng() % 0x801) - 0x400;
      if (!(rng() & 3)) z[i] = static_cast<s32>(rng() & 0xFFFFFF);
      da[i] = (rng() & 1 ? 0x10 : 0) | (rng() & 1 ? 0x00400000 : 0) | (rng() & 3 ? 0 : (rng() & 0xF));
    }
    std::memset(pa, 0xAA, sizeof pa); std::memset(pb, 0xAA, sizeof pb);
    const int mode = rng() & 3;
    const u32 ra = kern::ref::depth_candidates(mode, z, dz, da, n, pa), rb = N::depth_candidates(mode, z, dz, da, n, pb);
    if (ra != rb) { std::fprintf(stderr, "FAIL depth_candidates range %08x vs %08x (iteration %u)\n", ra, rb, it); ++failures; }
    CHECK_SAME("depth pass", pa, pb, n);
  }
}

int main() {
  test_depth_candidates();
  test_select16();
  test_resolve16();
  test_resolve16_one();
  test_resolve16_full();
  test_resolve16_top();
  test_composite_fade();
  test_rows16();
  test_obj_row16();
  test_palette();
  test_translucent_3d();
  test_composite();
  test_output();
  test_capture();
  test_scale_row();
  test_scale_row_grid();
  test_scale_row_straddle();
  test_scale_rows_cover();
  test_span();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
#if DSPERATE_NEON
  std::puts("kernels: ok (neon vs ref)");
#else
  std::puts("kernels: ok (ref only)");
#endif
  return 0;
}
