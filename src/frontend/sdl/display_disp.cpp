// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_disp.h"

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

bool DispOut::open(int rot, int screens, bool vsync) {
#if defined(__linux__)
  if (!available()) return false;
  if (rot != 0 && rot != 90 && rot != 180 && rot != 270) { std::fprintf(stderr, "disp: rotation %d not supported\n", rot); return false; }
  if (screens < 1 || screens > 2) return false;
  disp_ = ::open("/dev/disp", O_RDWR | O_CLOEXEC);
  fb_ = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (disp_ < 0 || fb_ < 0) { std::perror("disp: open"); close(); return false; }
  fb_fix_screeninfo fix{};
  if (ioctl(fb_, FBIOGET_FSCREENINFO, &fix) != 0) { std::perror("disp: FBIOGET_FSCREENINFO"); close(); return false; }
  unsigned long a[4] = {0, 0, 0, 0};
  panel_w_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_WIDTH, a));
  panel_h_ = static_cast<int>(ioctl(disp_, CMD_GET_SCN_HEIGHT, a));
  if (panel_w_ <= 0 || panel_h_ <= 0) { std::fprintf(stderr, "disp: no screen size\n"); close(); return false; }
  rot_ = rot; screens_ = screens; vsync_ = vsync;
  const bool turned = rot == 90 || rot == 270;
  comp_w_ = turned ? H * screens : W;
  comp_h_ = turned ? W : H * screens;
  comp_bytes_ = static_cast<size_t>(comp_w_) * comp_h_ * sizeof(u32);
  if (comp_bytes_ * BUFS > fix.smem_len) { std::fprintf(stderr, "disp: fb0 too small for %d composites\n", BUFS); close(); return false; }
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
  for (int b = 0; b < BUFS; ++b) fill_black(reinterpret_cast<u32*>(map_ + b * comp_bytes_));

  if (ui_layer_ >= 0) ui_was_enabled_ = layer_ioctl(disp_, CMD_LAYER_DISABLE, static_cast<unsigned>(ui_layer_), nullptr) == 0;
  cur_ = 0;
  if (!set_layer(phys_)) { close(); return false; }
  if (layer_ioctl(disp_, CMD_LAYER_ENABLE, static_cast<unsigned>(layer_), nullptr) != 0) { std::perror("disp: LAYER_ENABLE"); close(); return false; }
  next_ns_ = now_ns();
  displayed_ = 0; latched_ = pending_ = -1; stop_ = false;
  if (vsync_) thread_ = std::thread([this] { presenter(); });
  return true;
#else
  (void)rot; (void)screens; (void)vsync;
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
  disp_ = fb_ = -1; map_ = nullptr; map_len_ = 0; layer_ = ui_layer_ = -1; ui_was_enabled_ = false;
}

void DispOut::fill_black(u32* buf) {
  // Opaque black: the layer is in global-alpha mode, but keep the pixels sane
  // for anything that reads them back.
  const size_t n = static_cast<size_t>(comp_w_) * comp_h_;
  for (size_t i = 0; i < n; ++i) buf[i] = 0xFF000000u;
}

bool DispOut::set_layer(u32 addr) {
#if defined(__linux__)
  // Fit the composite to the panel, aspect kept, centred: the DE does the scale.
  const double s = std::min(static_cast<double>(panel_w_) / comp_w_, static_cast<double>(panel_h_) / comp_h_);
  const unsigned ww = static_cast<unsigned>(comp_w_ * s), wh = static_cast<unsigned>(comp_h_ * s);
  DispLayerInfo info{};
  info.mode = LAYER_MODE_SCALER;
  info.pipe = 1; info.zorder = 0; info.alpha_mode = 1; info.alpha_value = 255; info.ck_enable = 0;
  info.screen_win = {static_cast<int>((panel_w_ - ww) / 2), static_cast<int>((panel_h_ - wh) / 2), ww, wh};
  info.fb.addr[0] = addr;
  info.fb.size = {static_cast<unsigned>(comp_w_), static_cast<unsigned>(comp_h_)};
  info.fb.format = FORMAT_ARGB_8888;
  info.fb.src_win = {0, 0, static_cast<unsigned>(comp_w_), static_cast<unsigned>(comp_h_)};
  if (layer_ioctl(disp_, CMD_LAYER_SET_INFO, static_cast<unsigned>(layer_), &info) != 0) { std::perror("disp: LAYER_SET_INFO"); return false; }
  return true;
#else
  (void)addr; return false;
#endif
}

void DispOut::present(const u32* const fb[2]) {
#if defined(__linux__)
  if (!map_) return;
  int buf;
  if (thread_.joinable()) {
    std::lock_guard<std::mutex> g(mu_);
    if (pending_ >= 0) { buf = pending_; pending_ = -1; }   // not yet flipped: take it back and overwrite it (the panel skips that frame)
    else { buf = 0; while (buf == displayed_ || buf == latched_) ++buf; }
  } else buf = cur_ = (cur_ + 1) % BUFS;
  u32* comp = reinterpret_cast<u32*>(map_ + buf * comp_bytes_);
  for (int s = 0; s < screens_; ++s) {
    // Slot s's top-left in the composite. 270 puts the DS's top edge on the
    // panel's left edge, so the stack runs left to right; 90 runs the other
    // way; 0 and 180 stack vertically (180 upside down, so reversed).
    u32* dst;
    switch (rot_) {
      case 270: dst = comp + s * H; break;
      case 90:  dst = comp + (screens_ - 1 - s) * H; break;
      case 180: dst = comp + static_cast<size_t>((screens_ - 1 - s) * H) * comp_w_; break;
      default:  dst = comp + static_cast<size_t>(s * H) * comp_w_; break;
    }
    if (!fb[s]) {
      for (int y = 0; y < (rot_ == 90 || rot_ == 270 ? W : H); ++y)
        for (int x = 0; x < (rot_ == 90 || rot_ == 270 ? H : W); ++x) dst[y * comp_w_ + x] = 0xFF000000u;
      continue;
    }
    switch (rot_) {
      case 270: rot270(fb[s], dst, comp_w_); break;
      case 90:  rot90(fb[s], dst, comp_w_); break;
      case 180: rot180(fb[s], dst, comp_w_); break;
      default:  rot0(fb[s], dst, comp_w_); break;
    }
  }
  if (thread_.joinable()) {
    { std::lock_guard<std::mutex> g(mu_); pending_ = buf; }
    cv_.notify_one();
  } else set_layer(phys_ + static_cast<u32>(buf * comp_bytes_));
#else
  (void)fb;
#endif
}

void DispOut::presenter() {
#if defined(__linux__)
  // One flip per refresh: take the newest posted frame, flip to it, wait for
  // the refresh, and only then is the buffer it replaced free again.
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    cv_.wait(lk, [this] { return stop_ || pending_ >= 0; });
    if (stop_) return;
    latched_ = pending_; pending_ = -1;
    lk.unlock();
    set_layer(phys_ + static_cast<u32>(latched_ * comp_bytes_));
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
