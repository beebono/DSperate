// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// gpu_present_probe: drives the real GpuPresent (gpu_present.cpp, the render
// pass) into dma-heap buffers behind a stand-in scanout tier, and checks
// every pixel against a CPU model of the compute present it replaced
// (present.comp): rotation, two views at whole and fractional scales, a
// hidden view, the LCD grid, a fading PiP inset and the frontend overlay.
// Blends may differ by one step (fixed-function vs float mix); the rest must
// match exactly. Device-only (Vulkan + /dev/dma_heap), not a test.
#include "core/gpu/colour.h"
#include "frontend/sdl/gpu_present.h"
#include "frontend/sdl/dmaheap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace ds;
using namespace ds::sdl;

namespace {

constexpr int SW = 256, SH = 192;

// A two-buffer tier over dma-heap buffers, presented by nobody: the last
// end_frame()'d buffer is what a panel would show.
class HeapOut final : public ScanoutOut {
public:
  HeapOut(int w, int h) : w_(w), h_(h) {
    for (Buf& b : buf_) {
      b.len = static_cast<size_t>(w) * h * 4;
      b.fd = dmaheap::alloc(b.len, [](int) { return true; }, "gpu_present_probe");   // DS_DMA_HEAP picks the source
      if (b.fd < 0) continue;
      void* p = mmap(nullptr, b.len, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
      b.px = p == MAP_FAILED ? nullptr : static_cast<u32*>(p);
    }
  }
  ~HeapOut() override { for (Buf& b : buf_) { if (b.px) munmap(b.px, b.len); if (b.fd >= 0) ::close(b.fd); } }
  bool ok() const { return buf_[0].px && buf_[1].px; }
  bool reopen(SDL_Window*, int, int) override { return false; }
  void close() override {}
  int width() const override { return w_; }
  int height() const override { return h_; }
  int bufs() const override { return 2; }
  int current() const override { return cur_; }
  u32* begin_frame() override { cur_ = static_cast<int>(n_++ & 1); return buf_[cur_].px; }
  void end_frame() override { shown_ = cur_; cur_ = -1; }
  bool dmabuf_plane(int i, DmabufPlane& o) const override {
    if (i < 0 || i > 1 || buf_[i].fd < 0) return false;
    o.fd = buf_[i].fd; o.offset = 0; o.stride_bytes = static_cast<u32>(w_) * 4; o.width = static_cast<u32>(w_); o.height = static_cast<u32>(h_);
    o.fourcc = 0x34325258u;   // XR24
    return true;
  }
  // The shown buffer, read back through the cache.
  std::vector<u32> shown() const {
    const Buf& b = buf_[shown_ < 0 ? 0 : shown_];
    dma_buf_sync s{}; s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ; ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &s);
    std::vector<u32> v(b.px, b.px + b.len / 4);
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(b.fd, DMA_BUF_IOCTL_SYNC, &s);
    return v;
  }

private:
  struct Buf { int fd = -1; u32* px = nullptr; size_t len = 0; };
  Buf buf_[2];
  int w_, h_, cur_ = -1, shown_ = -1;
  u64 n_ = 0;
};

struct Case {
  const char* name;
  int rot, lw, lh;
  GpuPresent::View v[2];
  int nviews;
  u8 alpha = 255;
  u32 grid = 256;
  SDL_Rect over{0, 0, 0, 0};   // the overlay's drawn rectangle (filled with a pattern)
  ds::gpu::ColourProfile colour = ds::gpu::ColourProfile::Off;   // corrected on the GPU, modelled by kern::colour_pixel
  u32 filter = GpuPresent::kNearest;
};

// The CPU scanline scaler's filters (core/gpu/gpu.cpp), restated per panel pixel.
u32 lerp8(u32 a, u32 b, u32 f) {
  u32 r = 0;
  for (u32 sh = 0; sh < 32; sh += 8) r |= ((((a >> sh) & 255) * (256 - f) + ((b >> sh) & 255) * f + 128) >> 8) << sh;
  return r;
}
u32 lerp_linear(u32 a, u32 b, u32 f) {   // Gpu's GammaLut
  if (!f) return a;
  auto to_lin = [](u32 c) { const double v = c / 255.0; return static_cast<u32>(std::lround((v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)) * 4095.0)); };
  auto from_lin = [](u32 l) { const double v = l / 4095.0; return static_cast<u32>(std::lround((v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055) * 255.0)); };
  u32 r = 0xFF000000u;
  for (u32 sh = 0; sh < 24; sh += 8) r |= from_lin((to_lin((a >> sh) & 255) * (256 - f) + to_lin((b >> sh) & 255) * f + 128) >> 8) << sh;
  return r;
}
// Display::build_scale's lin_sx/lin_wx and Gpu::emit_bilinear's rows.
template <typename Px> u32 m_bilinear(Px px, int x, int y, int w, int h) {
  const s32 u = static_cast<s32>(((2 * x + 1) * SW * 128) / w) - 128;
  u32 sx = 0, fx = 0;
  if (u > 0) { sx = static_cast<u32>(u) >> 8; fx = static_cast<u32>(u) & 255; if (sx >= SW - 1) { sx = SW - 2; fx = 255; } }
  const s32 v = static_cast<s32>(((2 * y + 1) * SH * 128) / h) - 128;
  u32 sy = 0, fy = 0;
  if (v > 0) { sy = static_cast<u32>(v) >> 8; fy = static_cast<u32>(v) & 255; if (sy >= SH - 1) { sy = SH - 1; fy = 0; } }
  const u32 r0 = lerp8(px(sx, sy), px(sx + 1, sy), fx);
  return fy ? lerp8(r0, lerp8(px(sx, sy + 1), px(sx + 1, sy + 1), fx), fy) : r0;
}
// Gpu::emit_scaled's box-filter seams: seam_w, the straddling row (blend_rows), then emit_row_straddle.
template <typename Px> u32 m_seam(Px px, int x, int y, int w, int h, bool lin) {
  const u32 sx = static_cast<u32>(x * SW / w), sy = static_cast<u32>(y * SH / h);
  auto bl = [&](u32 a, u32 b, u32 f) { return lin ? lerp_linear(a, b, f) : lerp8(a, b, f); };
  const u32 tb = (sy + 1) * static_cast<u32>(h), vf = tb % SH;
  const bool vrow = sy + 1 < SH && vf && static_cast<u32>(y) == tb / SH;
  const u32 wl = ((SH - vf) * 256) / SH;
  const u32 hb = (sx + 1) * static_cast<u32>(w), hf = hb % SW;
  const bool hcol = sx + 1 < SW && hf && static_cast<u32>(x) == hb / SW;
  u32 c = px(sx, sy);
  if (vrow) c = bl(c, px(sx, sy + 1), wl);
  if (hcol) { u32 n = px(sx + 1, sy); if (vrow) n = bl(n, px(sx + 1, sy + 1), wl); c = bl(c, n, 256 - (hf * 256) / SW); }
  return c;
}

// gpu_present.cpp's LCD grid seam table (kern::scale_row_grid's placement).
void seams(const Case& c, u32 t[128]) {
  std::memset(t, 0, sizeof(u32) * 128);
  if (c.grid >= 256) return;
  for (int v = 0; v < std::min(c.nviews, 2); ++v) {
    if (!c.v[v].grid) continue;
    const int dim[2] = {c.v[v].rect.w, c.v[v].rect.h}, srcn[2] = {SW, SH};
    for (int axis = 0; axis < 2; ++axis) {
      const u32 n = static_cast<u32>(std::max(0, dim[axis])), sn = static_cast<u32>(srcn[axis]);
      if (n == 0 || n > 1024) continue;
      const u32 min_run = std::max<u32>(2, (n + sn - 1) / sn), pitch = c.grid == 0 && n == 2 * sn ? 2 : 1;
      for (u32 s = 0; s < sn; ++s) {
        const u32 x0 = (s * n + sn - 1) / sn, x1 = ((s + 1) * n + sn - 1) / sn;
        if (x1 - x0 >= min_run && s % pitch == 0 && x0 < n) t[v * 64 + axis * 32 + (x0 >> 5)] |= 1u << (x0 & 31);
      }
    }
  }
}

// present.comp, per panel pixel, in float as the shader did it.
std::vector<u32> model(const Case& c, int pw, int ph, const u32* const fb[2], const std::vector<u32>& over, const ds::gpu::kern::ColourLut* lut) {
  u32 tab[128]; seams(c, tab);
  auto seam = [&](int v, int d, int axis) { return d >= 0 && d < 1024 && ((tab[v * 64 + axis * 32 + (d >> 5)] >> (d & 31)) & 1u) != 0; };
  std::vector<u32> out(static_cast<size_t>(pw) * ph);
  for (int py = 0; py < ph; ++py)
    for (int px = 0; px < pw; ++px) {
      int lx, ly;
      if (c.rot == 90)       { lx = py;              ly = c.lh - 1 - px; }
      else if (c.rot == 180) { lx = c.lw - 1 - px;   ly = c.lh - 1 - py; }
      else if (c.rot == 270) { lx = c.lw - 1 - py;   ly = px; }
      else                   { lx = px;              ly = py; }
      float r = 0, g = 0, b = 0;
      for (int v = 0; v < c.nviews; ++v) {
        const SDL_Rect& rc = c.v[v].rect;
        if (!c.v[v].shown || lx < rc.x || ly < rc.y || lx >= rc.x + rc.w || ly >= rc.y + rc.h) continue;
        const int sx = (lx - rc.x) * SW / rc.w, sy = (ly - rc.y) * SH / rc.h;
        // Colour first (the present corrects as it uploads), then the filter.
        const u32* const f = fb[c.v[v].screen];
        auto px = [&](u32 x, u32 y) { const u32 p = f[y * SW + x]; return lut ? ds::gpu::kern::colour_pixel(*lut, p) : p; };
        const int ox = lx - rc.x, oy = ly - rc.y;
        const bool seams = c.filter >= GpuPresent::kSeamBlend && rc.h >= SH;
        const u32 s = c.filter == GpuPresent::kBilinear ? m_bilinear(px, ox, oy, rc.w, rc.h)
                    : seams ? m_seam(px, ox, oy, rc.w, rc.h, c.filter == GpuPresent::kSeamLinear) : px(sx, sy);
        float cr = static_cast<float>((s >> 16) & 255), cg = static_cast<float>((s >> 8) & 255), cb = static_cast<float>(s & 255);
        if (c.filter == GpuPresent::kNearest && c.grid < 256 && c.v[v].grid && (seam(v, lx - rc.x, 0) || seam(v, ly - rc.y, 1))) {
          const float f = static_cast<float>(c.grid) / 256.0f;
          cr = std::floor(cr * f); cg = std::floor(cg * f); cb = std::floor(cb * f);
        }
        cr /= 255; cg /= 255; cb /= 255;
        if (c.v[v].blends && c.alpha < 255) {
          const float k = static_cast<float>(c.alpha) / 255.0f;
          r += (cr - r) * k; g += (cg - g) * k; b += (cb - b) * k;
        } else { r = cr; g = cg; b = cb; }
      }
      if (c.over.w > 0) {
        const u32 ov = over[std::min(static_cast<size_t>(ly) * c.lw + lx, over.size() - 1)], oa = ov >> 24;
        if (oa) {
          const float k = static_cast<float>(oa) / 255.0f;
          r += (static_cast<float>((ov >> 16) & 255) / 255 - r) * k; g += (static_cast<float>((ov >> 8) & 255) / 255 - g) * k; b += (static_cast<float>(ov & 255) / 255 - b) * k;
        }
      }
      auto q = [](float x) { return static_cast<u32>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
      out[static_cast<size_t>(py) * pw + px] = q(r) << 16 | q(g) << 8 | q(b);
    }
  return out;
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::vector<u32> screen[2];
  u32 seed = 0x2468ace1u;
  for (auto& s : screen) { s.resize(SW * SH); for (u32& p : s) { seed = seed * 1664525u + 1013904223u; p = 0xFF000000u | (seed >> 8); } }
  const u32* fb[2] = {screen[0].data(), screen[1].data()};

  // Logical 1024x768 throughout: a 2x top screen and a fractional bottom one beside it.
  const GpuPresent::View two[2] = {{0, {0, 0, 512, 384}, true, false, false}, {1, {512, 300, 300, 200}, true, false, false}};
  std::vector<Case> cases;
  for (int rot : {0, 90, 180, 270}) {
    static char names[4][32];
    char* n = names[rot / 90]; std::snprintf(n, 32, "two views, rot %d", rot);
    Case c{n, rot, 1024, 768, {two[0], two[1]}, 2};
    cases.push_back(c);
  }
  { Case c{"one hidden view", 0, 1024, 768, {two[0], two[1]}, 2}; c.v[0].shown = false; cases.push_back(c); }
  { Case c{"LCD grid, 3x + 2x", 0, 1024, 768, {{0, {0, 0, 768, 576}, true, false, true}, {1, {768, 576, 256, 192}, true, false, true}}, 2}; c.grid = 128;
    c.v[1].rect = {600, 300, 512 - 88, 384 - 66}; cases.push_back(c); }
  { Case c{"LCD grid black, 2x", 90, 1024, 768, {{0, {0, 0, 512, 384}, true, false, true}, two[1]}, 2}; c.grid = 0; cases.push_back(c); }
  { Case c{"fading PiP inset", 0, 1024, 768, {{0, {0, 0, 1024, 768}, true, false, false}, {1, {700, 500, 256, 192}, true, true, false}}, 2}; c.alpha = 100; cases.push_back(c); }
  { Case c{"fading inset, rot 270", 270, 1024, 768, {{0, {0, 0, 1024, 768}, true, false, false}, {1, {700, 500, 256, 192}, true, true, false}}, 2}; c.alpha = 200; cases.push_back(c); }
  { Case c{"overlay", 0, 1024, 768, {two[0], two[1]}, 2}; c.over = {100, 250, 600, 200}; cases.push_back(c); }
  { Case c{"overlay, rot 90", 90, 1024, 768, {two[0], two[1]}, 2}; c.over = {900, 700, 300, 200}; cases.push_back(c); }   // clipped at the frame's edge
  { Case c{"colour nds, two views", 0, 1024, 768, {two[0], two[1]}, 2}; c.colour = ds::gpu::ColourProfile::Nds; cases.push_back(c); }
  { Case c{"colour nds_lite, rot 90", 90, 1024, 768, {two[0], two[1]}, 2}; c.colour = ds::gpu::ColourProfile::NdsLite; cases.push_back(c); }
  { Case c{"colour nds_lite + grid", 0, 1024, 768, {{0, {0, 0, 768, 576}, true, false, true}, {1, {768, 576, 256, 192}, true, false, true}}, 2};
    c.grid = 128; c.colour = ds::gpu::ColourProfile::NdsLite; cases.push_back(c); }
  // Fractional scales (2.5x as the RG DS Plus' 640x480 halves, 3.7x, 1.3x), so every seam weight shows; the grid must not.
  const GpuPresent::View frac[2] = {{0, {0, 0, 640, 480}, true, false, true}, {1, {640, 0, 384, 250}, true, false, true}};
  { Case c{"bilinear", 0, 1024, 768, {frac[0], frac[1]}, 2}; c.filter = GpuPresent::kBilinear; c.grid = 128; cases.push_back(c); }
  { Case c{"bilinear 3.7x, rot 90", 90, 1024, 768, {{0, {30, 20, 947, 710}, true, false, false}, frac[1]}, 2}; c.filter = GpuPresent::kBilinear; cases.push_back(c); }
  { Case c{"seam blend", 0, 1024, 768, {frac[0], frac[1]}, 2}; c.filter = GpuPresent::kSeamBlend; c.grid = 128; cases.push_back(c); }
  { Case c{"seam blend 3.7x + colour", 0, 1024, 768, {{0, {30, 20, 947, 710}, true, false, false}, frac[1]}, 2}; c.filter = GpuPresent::kSeamBlend; c.colour = ds::gpu::ColourProfile::Nds; cases.push_back(c); }
  { Case c{"seam blend, linear light", 0, 1024, 768, {frac[0], frac[1]}, 2}; c.filter = GpuPresent::kSeamLinear; cases.push_back(c); }

  bool pass = true;
  for (const Case& c : cases) {
    const bool swap = c.rot == 90 || c.rot == 270;
    const int pw = swap ? c.lh : c.lw, ph = swap ? c.lw : c.lh;
    HeapOut out(pw, ph);
    if (!out.ok()) { std::printf("no %dx%d dma-buf\n", pw, ph); return 1; }
    std::string why;
    auto gp = GpuPresent::open(out, &why);
    if (!gp) { std::printf("GpuPresent::open: %s\n", why.c_str()); return 1; }
    // The overlay: a pattern over its drawn rectangle with every alpha from clear to opaque.
    std::vector<u32> over(static_cast<size_t>(c.lw) * c.lh, 0);
    u32* canvas = gp->overlay(c.lw, c.lh);
    if (c.over.w > 0 && canvas) {
      for (int y = std::max(0, c.over.y); y < std::min(c.lh, c.over.y + c.over.h); ++y)
        for (int x = std::max(0, c.over.x); x < std::min(c.lw, c.over.x + c.over.w); ++x) {
          const u32 a = static_cast<u32>((x - c.over.x) * 255 / std::max(1, c.over.w - 1));
          over[static_cast<size_t>(y) * c.lw + x] = a << 24 | static_cast<u32>(y * 7 & 255) << 16 | static_cast<u32>(x * 3 & 255) << 8 | 0x40u;
        }
      std::memcpy(canvas, over.data(), over.size() * 4);
    }
    std::unique_ptr<ds::gpu::kern::ColourLut> lut;
    if (c.colour != ds::gpu::ColourProfile::Off) { lut = std::make_unique<ds::gpu::kern::ColourLut>(); ds::gpu::build_colour_lut(c.colour, *lut); }
    if (!gp->present(out, fb, c.v, c.nviews, c.rot, c.lw, c.lh, c.alpha, c.over, c.grid, nullptr, lut.get(), c.filter)) { std::printf("%s: present failed\n", c.name); pass = false; continue; }
    gp->flush(out);
    const std::vector<u32> got = out.shown(), want = model(c, pw, ph, fb, over, lut.get());
    u32 off1 = 0, bad = 0, fx = 0, fy = 0, fg = 0, fw = 0;
    for (int y = 0; y < ph; ++y)
      for (int x = 0; x < pw; ++x) {
        const u32 g = got[static_cast<size_t>(y) * pw + x] & 0xFFFFFFu, w = want[static_cast<size_t>(y) * pw + x];
        if (g == w) continue;
        int d = 0;
        for (int sh : {0, 8, 16}) d = std::max(d, std::abs(static_cast<int>((g >> sh) & 255) - static_cast<int>((w >> sh) & 255)));
        if (d <= 1 || (c.filter != GpuPresent::kNearest && d <= 3)) { ++off1; continue; }   // filters: the sampler's 8-bit weights (and sRGB decode), a few steps at worst
        if (!bad++) { fx = static_cast<u32>(x); fy = static_cast<u32>(y); fg = g; fw = w; }
      }
    const bool blends = c.over.w > 0 || c.alpha < 255 || c.filter != GpuPresent::kNearest;   // filters: the sampler's 8-bit weights vs the CPU's integer steps
    const bool ok = bad == 0 && (blends || off1 == 0);
    pass &= ok;
    std::printf("%-24s %4dx%-4d %s", c.name, pw, ph, ok ? "ok" : "MISMATCH");
    if (off1) std::printf(", %u pixels %s off%s", off1, c.filter != GpuPresent::kNearest ? "1-3 steps" : "one step", blends ? " (blend rounding)" : "");
    if (bad) std::printf(", %u pixels wrong, first at %u,%u: got %06x want %06x", bad, fx, fy, fg, fw);
    std::printf("\n");
  }
  std::printf("%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
