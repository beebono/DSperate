// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_disp.h"
#include "display.h"

#include "core/gpu/gpu.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <ctime>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define DS_DISP_NEON 1
#endif

namespace ds::sdl {

namespace {

// ---- disp 1.5 ABI (kernel-headers/drv_display.h) -------------------------------
constexpr unsigned CMD_LAYER_ENABLE = 0x40, CMD_LAYER_DISABLE = 0x41, CMD_LAYER_SET_INFO = 0x42, CMD_LAYER_GET_INFO = 0x43;
constexpr unsigned CMD_GET_SCN_WIDTH = 0x07, CMD_GET_SCN_HEIGHT = 0x08;
constexpr unsigned FBIOGET_LAYER_HDL_0 = 0x4700;
constexpr unsigned LAYER_MODE_SCALER = 4;
constexpr unsigned FORMAT_ARGB_8888 = 0;

struct DispWindow { int x, y; unsigned width, height; };
struct DispSize { unsigned width, height; };
struct DispFbInfo {
  unsigned addr[3];
  DispSize size;
  unsigned format;
  unsigned cs_mode;
  int      b_trd_src;
  unsigned trd_mode;
  unsigned trd_right_addr[3];
  int      pre_multiply;
  DispWindow src_win;
};
struct DispLayerInfo {
  unsigned mode;
  unsigned char pipe, zorder, alpha_mode, alpha_value;
  int      ck_enable;
  DispWindow screen_win;
  DispFbInfo fb;
  int      b_trd_out;
  unsigned out_trd_mode;
  unsigned id;
};
static_assert(sizeof(DispLayerInfo) == 108, "disp_layer_info layout (verified against the A30 driver)");

#if defined(__linux__)
long layer_ioctl(int fd, unsigned cmd, unsigned layer, void* info) {
  unsigned long args[4] = {0, layer, reinterpret_cast<unsigned long>(info), 0};
  return ioctl(fd, cmd, args);
}
u64 now_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return static_cast<u64>(t.tv_sec) * 1000000000ull + static_cast<u64>(t.tv_nsec); }
#endif

// ---- rotation kernels ----------------------------------------------------------
// src: SCREEN_W x SCREEN_H; dst: the slot's top-left in the composite, stride in
// pixels. 270: dst[py][px] = src[px][W-1-py] (the DS's top edge becomes the
// panel's left edge, which on a panel mounted 270 degrees is the user's top).
// 90: dst[py][px] = src[H-1-px][py]. The panel memory is uncached, so every
// store is a full 64-byte line: 16 source rows x 4 columns per step, four
// 4x4 NEON transposes, one 16-pixel column store each.
constexpr int W = static_cast<int>(SCREEN_W), H = static_cast<int>(SCREEN_H);

#if DS_DISP_NEON
inline void tr4(uint32x4_t r0, uint32x4_t r1, uint32x4_t r2, uint32x4_t r3, uint32x4_t& c0, uint32x4_t& c1, uint32x4_t& c2, uint32x4_t& c3) {
  const uint32x4x2_t a = vtrnq_u32(r0, r1), b = vtrnq_u32(r2, r3);
  c0 = vcombine_u32(vget_low_u32(a.val[0]), vget_low_u32(b.val[0]));
  c1 = vcombine_u32(vget_low_u32(a.val[1]), vget_low_u32(b.val[1]));
  c2 = vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(b.val[0]));
  c3 = vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(b.val[1]));
}
void rot270(const u32* src, u32* dst, int stride) {
  for (int sy = 0; sy < H; sy += 16) {
    const u32* r = src + sy * W;
    for (int sx = 0; sx < W; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * W + sx;
        tr4(vld1q_u32(p), vld1q_u32(p + W), vld1q_u32(p + 2 * W), vld1q_u32(p + 3 * W), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + (W - 1 - (sx + j)) * stride + sy;
        vst1q_u32(o, c[j][0]); vst1q_u32(o + 4, c[j][1]); vst1q_u32(o + 8, c[j][2]); vst1q_u32(o + 12, c[j][3]);
      }
    }
  }
}
void rot90(const u32* src, u32* dst, int stride) {
  for (int sy = 0; sy < H; sy += 16) {
    const u32* r = src + sy * W;
    for (int sx = 0; sx < W; sx += 4) {
      uint32x4_t c[4][4];
      for (int g = 0; g < 4; ++g) {
        const u32* p = r + g * 4 * W + sx;
        tr4(vld1q_u32(p + 3 * W), vld1q_u32(p + 2 * W), vld1q_u32(p + W), vld1q_u32(p), c[0][g], c[1][g], c[2][g], c[3][g]);
      }
      for (int j = 0; j < 4; ++j) {
        u32* o = dst + (sx + j) * stride + (H - 16 - sy);
        vst1q_u32(o, c[j][3]); vst1q_u32(o + 4, c[j][2]); vst1q_u32(o + 8, c[j][1]); vst1q_u32(o + 12, c[j][0]);
      }
    }
  }
}
#else
void rot270(const u32* src, u32* dst, int stride) {
  for (int py = 0; py < W; ++py) for (int px = 0; px < H; ++px) dst[py * stride + px] = src[px * W + (W - 1 - py)];
}
void rot90(const u32* src, u32* dst, int stride) {
  for (int py = 0; py < W; ++py) for (int px = 0; px < H; ++px) dst[py * stride + px] = src[(H - 1 - px) * W + py];
}
#endif
// 0 and 180 keep the DS orientation: whole rows, 64-byte stores either way.
void rot0(const u32* src, u32* dst, int stride) {
  for (int y = 0; y < H; ++y) std::memcpy(dst + y * stride, src + y * W, W * sizeof(u32));
}
void rot180(const u32* src, u32* dst, int stride) {
  for (int y = 0; y < H; ++y) {
    const u32* s = src + (H - 1 - y) * W;
    u32* d = dst + y * stride;
    for (int x = 0; x < W; ++x) d[x] = s[W - 1 - x];
  }
}

} // namespace

bool DispOut::available() {
#if defined(__linux__)
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  cached = 0;
  const int d = ::open("/dev/disp", O_RDWR | O_CLOEXEC);
  if (d < 0) return false;
  DispLayerInfo info{};
  const bool ok = layer_ioctl(d, CMD_LAYER_GET_INFO, 0, &info) == 0 && ::access("/dev/fb0", R_OK | W_OK) == 0;
  ::close(d);
  cached = ok ? 1 : 0;
  return ok;
#else
  return false;
#endif
}

bool DispOut::open(int rot, bool vsync) {
#if defined(__linux__)
  if (!available()) return false;
  if (rot != 0 && rot != 90 && rot != 180 && rot != 270) { std::fprintf(stderr, "disp: rotation %d not supported\n", rot); return false; }
  disp_ = ::open("/dev/disp", O_RDWR | O_CLOEXEC);
  fb_ = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (disp_ < 0 || fb_ < 0) { std::perror("disp: open"); close(); return false; }
  fb_fix_screeninfo fix{};
  if (ioctl(fb_, FBIOGET_FSCREENINFO, &fix) != 0) { std::perror("disp: FBIOGET_FSCREENINFO"); close(); return false; }
  unsigned long a[4] = {0, 0, 0, 0};
  panel_w_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_WIDTH, a));
  panel_h_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_HEIGHT, a));
  if (panel_w_ <= 0 || panel_h_ <= 0) { std::fprintf(stderr, "disp: no screen size\n"); close(); return false; }
  rot_ = rot; vsync_ = vsync;
  buf_bytes_ = static_cast<size_t>(W) * H * VIEWS * sizeof(u32);   // the largest canvas: two screens
  if (buf_bytes_ * BUFS > fix.smem_len) { std::fprintf(stderr, "disp: fb0 too small for %d composites\n", BUFS); close(); return false; }
  map_len_ = fix.smem_len;
  void* m = mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_, 0);
  if (m == MAP_FAILED) { std::perror("disp: mmap fb0"); map_ = nullptr; close(); return false; }
  map_ = static_cast<u8*>(m);
  phys_ = static_cast<u32>(fix.smem_start);

  // The UI's layer (the one fb0 drives) goes off while the panel is ours; a
  // free layer carries the composite. Both come back on close().
  unsigned long hdl = 0;
  if (ioctl(fb_, FBIOGET_LAYER_HDL_0, &hdl) == 0) ui_layer_ = static_cast<int>(hdl);
  for (int l = 0; l < 4; ++l) if (l != ui_layer_) { layer_ = l; break; }
  for (auto& v : views_) v = ViewRect{};
  for (auto& d : dims_) d = Dims{};
  canvas_w_ = W; canvas_h_ = H;
  dirty_ = (1u << BUFS) - 1;
  const size_t n = buf_bytes_ / sizeof(u32);
  for (int b = 0; b < BUFS; ++b) { u32* p = buf_ptr(b); for (size_t i = 0; i < n; ++i) p[i] = 0xFF000000u; }

  if (ui_layer_ >= 0) ui_was_enabled_ = layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(ui_layer_), nullptr) == 0;
  cur_ = 0;
  next_ns_ = now_ns();
  displayed_ = 0; latched_ = pending_ = -1; stop_ = false;
  if (vsync_) thread_ = std::thread([this] { presenter(); });
  return true;
#else
  (void)rot; (void)vsync;
  return false;
#endif
}

void DispOut::close() {
#if defined(__linux__)
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); stop_ = true; }
    cv_.notify_all();
    thread_.join();
  }
  if (disp_ >= 0) {
    if (layer_ >= 0) layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(layer_), nullptr);
    if (ui_layer_ >= 0 && ui_was_enabled_) layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(ui_layer_), nullptr);
    ::close(disp_);
  }
  if (map_) munmap(map_, map_len_);
  if (fb_ >= 0) ::close(fb_);
#endif
  disp_ = fb_ = -1; map_ = nullptr; map_len_ = 0; layer_ = ui_layer_ = -1; ui_was_enabled_ = false; layer_enabled_ = false;
}

void DispOut::set_canvas(int w, int h) {
  if (w <= 0 || h <= 0 || static_cast<size_t>(w) * h > static_cast<size_t>(W) * H * VIEWS) return;
  if (w == canvas_w_ && h == canvas_h_) return;
  canvas_w_ = w; canvas_h_ = h;
  dirty_ = (1u << BUFS) - 1;   // gaps between views must be black again
}

void DispOut::set_view(int i, int x, int y, int w, int h, bool shown) {
  if (i < 0 || i >= VIEWS) return;
  const ViewRect& o = views_[i];
  // A moved, resized or hidden view leaves its old pixels behind in every
  // buffer: pip to single, a screen swap or a corner change keep the canvas
  // size, so set_canvas() alone would not black them out.
  if (o.x != x || o.y != y || o.w != w || o.h != h || o.shown != shown) dirty_ = (1u << BUFS) - 1;
  views_[i] = ViewRect{x, y, w, h, shown};
}

bool DispOut::set_layer(u32 addr, Dims d) {
#if defined(__linux__)
  if (d.w <= 0 || d.h <= 0) return false;
  // Fit the composite to the panel, aspect kept, centred: the DE does the scale.
  const double s = std::min(static_cast<double>(panel_w_) / d.w, static_cast<double>(panel_h_) / d.h);
  const unsigned ww = static_cast<unsigned>(d.w * s), wh = static_cast<unsigned>(d.h * s);
  DispLayerInfo info{};
  info.mode = LAYER_MODE_SCALER;
  info.pipe = 1; info.zorder = 0; info.alpha_mode = 1; info.alpha_value = 255; info.ck_enable = 0;
  info.screen_win = {static_cast<int>((panel_w_ - ww) / 2), static_cast<int>((panel_h_ - wh) / 2), ww, wh};
  info.fb.addr[0] = addr;
  info.fb.size = {static_cast<unsigned>(d.w), static_cast<unsigned>(d.h)};
  info.fb.format = FORMAT_ARGB_8888;
  info.fb.src_win = {0, 0, static_cast<unsigned>(d.w), static_cast<unsigned>(d.h)};
  if (layer_ioctl(disp_, CMD_LAYER_SET_INFO, static_cast<unsigned>(layer_), &info) != 0) { std::perror("disp: LAYER_SET_INFO"); return false; }
  if (!layer_enabled_) {
    if (layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(layer_), nullptr) != 0) { std::perror("disp: LAYER_ENABLE"); return false; }
    layer_enabled_ = true;
  }
  return true;
#else
  (void)addr; (void)d; return false;
#endif
}

void DispOut::flip(int buf) { set_layer(buf_addr(buf), dims_[buf]); }

// One view into the composite. A 1:1 view takes the NEON rotate; any other
// size is a box downscale (area average over the source block each output
// pixel covers) into a cached temporary, rotated there, and copied in by
// rows so the uncached panel memory sees whole lines.
void DispOut::canvas_point(int compx, int compy, int& x, int& y) const {
  switch (rot_) {
    case 270: x = canvas_w_ - 1 - compy; y = compx; break;
    case 90:  x = compy; y = canvas_h_ - 1 - compx; break;
    case 180: x = canvas_w_ - 1 - compx; y = canvas_h_ - 1 - compy; break;
    default:  x = compx; y = compy; break;
  }
}

const u32* DispOut::under_pixel(int x, int y, int index, const u32* const fbs[VIEWS]) const {
  for (int v = index - 1; v >= 0; --v) {
    const ViewRect& u = views_[v];
    if (!fbs[v] || !u.shown || u.w != W || u.h != H) continue;
    if (x < u.x || x >= u.x + u.w || y < u.y || y >= u.y + u.h) continue;
    return fbs[v] + static_cast<size_t>(y - u.y) * W + (x - u.x);
  }
  return nullptr;
}

void DispOut::draw_view(u32* comp, int comp_w, const ViewRect& r, const u32* fb, int index, const u32* const fbs[VIEWS]) {
  const bool turned = rot_ == 90 || rot_ == 270;
  // The view's top-left in the composite: the same rotation the pixels get.
  int cx, cy;
  switch (rot_) {
    case 270: cx = r.y; cy = canvas_w_ - (r.x + r.w); break;
    case 90:  cx = canvas_h_ - (r.y + r.h); cy = r.x; break;
    case 180: cx = canvas_w_ - (r.x + r.w); cy = canvas_h_ - (r.y + r.h); break;
    default:  cx = r.x; cy = r.y; break;
  }
  u32* dst = comp + static_cast<size_t>(cy) * comp_w + cx;
  if (r.w == W && r.h == H) {
    switch (rot_) {
      case 270: rot270(fb, dst, comp_w); break;
      case 90:  rot90(fb, dst, comp_w); break;
      case 180: rot180(fb, dst, comp_w); break;
      default:  rot0(fb, dst, comp_w); break;
    }
    return;
  }
  if (r.w <= 0 || r.h <= 0 || r.w > W || r.h > H) return;
  // Downscale, unrotated, into tmp_ (r.w x r.h).
  tmp_.resize(static_cast<size_t>(r.w) * r.h);
  for (int dy = 0; dy < r.h; ++dy) {
    const int sy0 = dy * H / r.h, sy1 = std::max(sy0 + 1, (dy + 1) * H / r.h);
    for (int dx = 0; dx < r.w; ++dx) {
      const int sx0 = dx * W / r.w, sx1 = std::max(sx0 + 1, (dx + 1) * W / r.w);
      unsigned rs = 0, gs = 0, bs = 0, n = 0;
      for (int sy = sy0; sy < sy1; ++sy) for (int sx = sx0; sx < sx1; ++sx) {
        const u32 p = fb[sy * W + sx];
        rs += (p >> 16) & 0xFF; gs += (p >> 8) & 0xFF; bs += p & 0xFF; ++n;
      }
      tmp_[static_cast<size_t>(dy) * r.w + dx] = 0xFF000000u | ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
    }
  }
  // Rotate into tmp2_ (tw x th) with the same mapping the 1:1 kernels use.
  const int tw = turned ? r.h : r.w, th = turned ? r.w : r.h;
  tmp2_.resize(static_cast<size_t>(tw) * th);
  for (int py = 0; py < th; ++py) for (int px = 0; px < tw; ++px) {
    int vx, vy;
    switch (rot_) {
      case 270: vx = r.w - 1 - py; vy = px; break;
      case 90:  vx = py; vy = r.h - 1 - px; break;
      case 180: vx = r.w - 1 - px; vy = r.h - 1 - py; break;
      default:  vx = px; vy = py; break;
    }
    tmp2_[static_cast<size_t>(py) * tw + px] = tmp_[static_cast<size_t>(vy) * r.w + vx];
  }
  // Into the composite by rows: copied, or blended over the view under it
  // (drawn first: views go in order) when the inset is translucent. The
  // pixels under it come from that view's own framebuffer, not the
  // composite: the panel memory is uncached, and reading a small inset
  // back through it cost 0.6 ms a frame on the A30. Where no 1:1 view lies
  // under a pixel (nothing does in the layouts we have) the composite is
  // read after all.
  if (inset_alpha_ == 255) {
    for (int py = 0; py < th; ++py) std::memcpy(dst + static_cast<size_t>(py) * comp_w, tmp2_.data() + static_cast<size_t>(py) * tw, static_cast<size_t>(tw) * sizeof(u32));
    return;
  }
  tmp3_.resize(static_cast<size_t>(tw));
  // A composite row is a straight line on the canvas: its step per pixel.
  const int sx = rot_ == 0 ? 1 : rot_ == 180 ? -1 : 0, sy = rot_ == 270 ? 1 : rot_ == 90 ? -1 : 0;
  for (int py = 0; py < th; ++py) {
    u32* row = dst + static_cast<size_t>(py) * comp_w;
    int x0, y0;
    canvas_point(cx, cy + py, x0, y0);
    // Usual case: the whole row lies in one 1:1 view -- a strided gather from
    // its (cached) framebuffer. Otherwise pixel by pixel, the composite
    // read back where nothing is under it.
    const u32* u0 = under_pixel(x0, y0, index, fbs);
    const u32* u1 = under_pixel(x0 + sx * (tw - 1), y0 + sy * (tw - 1), index, fbs);
    if (u0 && u1 && (u1 - u0) == static_cast<std::ptrdiff_t>(sx + sy * static_cast<int>(W)) * (tw - 1)) {
      const std::ptrdiff_t step = sx + sy * static_cast<int>(W);
      for (int px = 0; px < tw; ++px) tmp3_[static_cast<size_t>(px)] = u0[step * px];
    } else {
      for (int px = 0; px < tw; ++px) {
        const u32* u = under_pixel(x0 + sx * px, y0 + sy * px, index, fbs);
        tmp3_[static_cast<size_t>(px)] = u ? *u : row[px];
      }
    }
    Display::blend_row(tmp3_.data(), tmp2_.data() + static_cast<size_t>(py) * tw, static_cast<size_t>(tw), inset_alpha_);
    std::memcpy(row, tmp3_.data(), static_cast<size_t>(tw) * sizeof(u32));
  }
}

void DispOut::present(const u32* const fb[VIEWS]) {
#if defined(__linux__)
  if (!map_) return;
  int buf;
  if (thread_.joinable()) {
    std::lock_guard<std::mutex> g(mu_);
    if (pending_ >= 0) { buf = pending_; pending_ = -1; }   // not yet flipped: take it back and overwrite it (the panel skips that frame)
    else { buf = 0; while (buf == displayed_ || buf == latched_) ++buf; }
  } else buf = cur_ = (cur_ + 1) % BUFS;
  const bool turned = rot_ == 90 || rot_ == 270;
  const Dims d{turned ? canvas_h_ : canvas_w_, turned ? canvas_w_ : canvas_h_};
  u32* comp = buf_ptr(buf);
  if (dirty_ & (1u << buf)) {
    const size_t n = static_cast<size_t>(d.w) * d.h;
    for (size_t i = 0; i < n; ++i) comp[i] = 0xFF000000u;
    dirty_ &= ~(1u << buf);
  }
  for (int v = 0; v < VIEWS; ++v) if (fb[v] && views_[v].shown) draw_view(comp, d.w, views_[v], fb[v], v, fb);
  dims_[buf] = d;
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); pending_ = buf; }
    cv_.notify_one();
  } else flip(buf);
#else
  (void)fb;
#endif
}

void DispOut::presenter() {
#if defined(__linux__)
  // One flip per refresh: take the newest posted frame, flip to it, wait for
  // the refresh, and only then is the buffer it replaced free again. The
  // ioctls run outside the lock so a post never waits on them.
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    cv_.wait(lk, [this] { return stop_ || pending_ >= 0; });
    if (stop_) return;
    latched_ = pending_; pending_ = -1;
    lk.unlock();
    flip(latched_);
    wait_vsync();
    lk.lock();
    displayed_ = latched_; latched_ = -1;
  }
#endif
}

void DispOut::wait_vsync() {
#if defined(__linux__)
  // FBIOPAN_DISPLAY on fb0 blocks until the next refresh on this driver (its
  // FBIO_WAITFORVSYNC returns at once), even with the UI layer off -- checked
  // the first time through; if it ever returns early, pace by the clock.
  if (pan_blocks_) {
    fb_var_screeninfo var{};
    if (ioctl(fb_, FBIOGET_VSCREENINFO, &var) == 0) {
      const u64 t0 = now_ns();
      ioctl(fb_, FBIOPAN_DISPLAY, &var);
      if (!pan_measured_) {
        pan_measured_ = true;
        const u64 dt = now_ns() - t0;
        if (dt < 1000000ull) {
          pan_blocks_ = false;
          std::fprintf(stderr, "disp: FBIOPAN_DISPLAY does not wait for vsync; pacing by the clock\n");
        }
      }
      if (pan_blocks_) return;
    } else pan_blocks_ = false;
  }
  constexpr u64 PERIOD = 16666667ull;
  const u64 t = now_ns();
  if (next_ns_ < t - PERIOD) next_ns_ = t;
  next_ns_ += PERIOD;
  if (next_ns_ > t) {
    const u64 d = next_ns_ - t;
    timespec ts{static_cast<time_t>(d / 1000000000ull), static_cast<long>(d % 1000000000ull)};
    nanosleep(&ts, nullptr);
  }
#endif
}

} // namespace ds::sdl
