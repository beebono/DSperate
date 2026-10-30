// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// RGA present stage: the Rockchip 2D accelerator (a V4L2 mem2mem device,
// /dev/videoN "rockchip-rga") scales the core's 256x192 frames into the
// scanout sink's own dma-buf, one job per shown view, with crop on the
// output queue and compose on the capture queue for the layout. Neither the
// CPU nor the GPU touches a panel-sized pixel: the CPU copies the two 1x
// frames into small dma-buf source buffers (192 KB each), the RGA does the rest
// in ~1 ms a view, and the present thread waits on it with poll().
//
// The driver's scaler filters (bilinear; there is no filter control), so
// with video.linear off a view at a whole-number scale k takes an exact
// path instead, where no job scales: the CPU repeats each pixel k times
// across into a 256k x 192 strip, and k 1:1 jobs repeat its rows. Those
// jobs describe the panel buffer as k*W wide and H/k tall, which puts k
// physical rows side by side in one row of the description, so a job
// composed at x = j*W lands on physical rows k*s + j. The strip of the next
// view is written while the RGA runs the last job of the one before. At 4x
// on 1024x768 a view costs ~0.9 ms of CPU and ~2.7 ms of RGA (bilinear:
// ~1.8 ms of RGA); tests/rga_nearest_probe.cpp measures it. Fractional
// scales, and views the exact path cannot place, stay bilinear.
//
// What the RGA cannot do is done by the CPU on the finished buffer, over
// the pixels concerned only: the frontend's overlay (OSD, pause menu) over
// the rectangle it drew; a fading PiP inset, which the RGA scales into a
// scratch buffer and the CPU blends in; the LCD grid's seam columns and
// rows (the same placement as the scanline scaler's). Seams and chunky
// cells do not exist on this route (the menu greys them out). Rotation:
// the presenter refuses to open and the scanline scaler handles rotated
// panels.
#include "frontend/sdl/video/presenter.h"
#include "frontend/sdl/scanout.h"
#include "frontend/sdl/dmaheap.h"

#include <SDL.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if DSPERATE_NEON
#include <arm_neon.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
// The V4L2 this needs: multi-planar mem2mem, dma-buf buffers, XBGR32. Older
// kernel headers (the A30's) lack them, and those devices have no RGA anyway.
#if defined(V4L2_PIX_FMT_XBGR32) && defined(V4L2_CAP_VIDEO_M2M_MPLANE)   // (V4L2_MEMORY_DMABUF is an enumerator, of the same vintage)
#define DS_RGA_V4L2 1
#endif
#endif

namespace ds::sdl {

#if DS_RGA_V4L2
namespace {

constexpr int kSlots = 2;               // frames in flight: the one being composed and the one the RGA reads
constexpr u32 kFrameBytes = ds::SCREEN_W * ds::SCREEN_H * 4;
constexpr u32 kMaxK = 16;               // exact path: the widest scale (the driver's formats stop at 8192 px)
constexpr int kMinSide = 34;            // the driver's smallest crop / compose side

int xioctl(int fd, unsigned long req, void* arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; }

bool v4l_reqbufs(int fd, u32 type, u32 n) {
  v4l2_requestbuffers rb{}; rb.type = static_cast<v4l2_buf_type>(type); rb.memory = V4L2_MEMORY_DMABUF; rb.count = n;
  return xioctl(fd, VIDIOC_REQBUFS, &rb) == 0 && rb.count >= n;
}
bool v4l_stream(int fd, u32 type, bool on) { int tt = static_cast<int>(type); return xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &tt) == 0; }
bool v4l_select(int fd, u32 type, u32 target, const frontend::Rect& r) {
  v4l2_selection s{}; s.type = type; s.target = target; s.r.left = r.x; s.r.top = r.y; s.r.width = static_cast<u32>(r.w); s.r.height = static_cast<u32>(r.h);
  return xioctl(fd, VIDIOC_S_SELECTION, &s) == 0;
}
bool v4l_queue(int fd, u32 type, int dmafd, u32 bytes) {
  v4l2_plane pl{}; pl.m.fd = dmafd; pl.length = bytes; if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) pl.bytesused = bytes;
  v4l2_buffer b{}; b.type = static_cast<v4l2_buf_type>(type); b.memory = V4L2_MEMORY_DMABUF; b.index = 0; b.m.planes = &pl; b.length = 1;
  return xioctl(fd, VIDIOC_QBUF, &b) == 0;
}
bool v4l_dequeue(int fd, u32 type) {
  v4l2_plane pl{};
  v4l2_buffer b{}; b.type = static_cast<v4l2_buf_type>(type); b.memory = V4L2_MEMORY_DMABUF; b.m.planes = &pl; b.length = 1;
  return xioctl(fd, VIDIOC_DQBUF, &b) == 0;
}
// XBGR32 at exactly w x h; the stride and image size the driver settled on.
bool v4l_format(int fd, u32 type, u32 w, u32 h, u32* stride, u32* bytes) {
  v4l2_format f{}; f.type = static_cast<v4l2_buf_type>(type);
  f.fmt.pix_mp.width = w; f.fmt.pix_mp.height = h; f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_XBGR32; f.fmt.pix_mp.field = V4L2_FIELD_NONE; f.fmt.pix_mp.num_planes = 1;
  if (xioctl(fd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix_mp.width != w || f.fmt.pix_mp.height != h || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_XBGR32) return false;
  *stride = f.fmt.pix_mp.plane_fmt[0].bytesperline; *bytes = f.fmt.pix_mp.plane_fmt[0].sizeimage;
  return true;
}

// The RGA node: DS_RGA_DEVICE, else the first /dev/video* whose driver is rockchip-rga.
int open_rga(std::string* why, std::string* path) {
  auto try_open = [](const char* p) -> int {
    int fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 || std::strcmp(reinterpret_cast<const char*>(cap.driver), "rockchip-rga") != 0 ||
        !(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)) { close(fd); return -1; }
    return fd;
  };
  if (const char* d = std::getenv("DS_RGA_DEVICE")) {
    const int fd = try_open(d);
    if (fd < 0 && why) *why = std::string(d) + " is not a rockchip-rga mem2mem device";
    if (fd >= 0) *path = d;
    return fd;
  }
  for (int i = 0; i < 16; ++i) {
    char p[32]; std::snprintf(p, sizeof p, "/dev/video%d", i);
    const int fd = try_open(p);
    if (fd >= 0) { *path = p; return fd; }
  }
  if (why) *why = "no rockchip-rga V4L2 device";
  return -1;
}

// Each pixel of the frame k times across: the exact path's 256k x 192 strip
// (no row padding, so one run over the whole frame). The CPU's write rate
// to RAM bounds it, ~1 GB/s on the RK3566, not the instructions.
void repeat_columns(const u32* s, u32* d, u32 k) {
  const size_t n = static_cast<size_t>(ds::SCREEN_W) * ds::SCREEN_H;
  if (k == 1) { std::memcpy(d, s, n * 4); return; }
  size_t x = 0;
#if DSPERATE_NEON
  // One interleaving store of k copies of a vector writes its 4 pixels k times each.
  if (k == 2)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst2q_u32(d + 2 * x, (uint32x4x2_t{{a, a}})); vst2q_u32(d + 2 * x + 8, (uint32x4x2_t{{b, b}}));
    }
  else if (k == 3)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst3q_u32(d + 3 * x, (uint32x4x3_t{{a, a, a}})); vst3q_u32(d + 3 * x + 12, (uint32x4x3_t{{b, b, b}}));
    }
  else if (k == 4)
    for (; x + 8 <= n; x += 8) {
      const uint32x4_t a = vld1q_u32(s + x), b = vld1q_u32(s + x + 4);
      vst4q_u32(d + 4 * x, (uint32x4x4_t{{a, a, a, a}})); vst4q_u32(d + 4 * x + 16, (uint32x4x4_t{{b, b, b, b}}));
    }
#endif
  for (; x < n; ++x) for (u32 i = 0; i < k; ++i) d[x * k + i] = s[x];
}

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

class RgaPresenter final : public FramePresenter {
public:
  static std::unique_ptr<FramePresenter> open(ScanoutOut& sink, std::string* why) {
    std::unique_ptr<RgaPresenter> p(new RgaPresenter(sink));
    if (!p->init(why)) return nullptr;
    return p;
  }
  ~RgaPresenter() override {
    sink_.set_gpu_writes(false);
    if (fd_ >= 0) { stream(false); close(fd_); }
    close_rows();
    for (Src& s : src_) for (int k = 0; k < frontend::SCREENS; ++k) { if (s.px[k]) munmap(s.px[k], kFrameBytes); if (s.fd[k] >= 0) close(s.fd[k]); }
    for (Strip& s : strip_) { if (s.px) munmap(s.px, s.bytes); if (s.fd >= 0) close(s.fd); }
    if (scratch_px_) munmap(scratch_px_, scratch_bytes_);
    if (scratch_fd_ >= 0) close(scratch_fd_);
  }

  Kind kind() const override { return Kind::Rga; }
  Fit fit(SDL_Window* win, int w, int h) override {
    if (w == sink_.width() && h == sink_.height()) return Fit::Same;
    std::string why;
    close_rows();   // their capture side describes the old buffer
    if (sink_.reopen(win, w, h) && setup_capture(&why)) { cleared_.assign(static_cast<size_t>(std::max(0, sink_.bufs())), false); return Fit::Changed; }
    std::fprintf(stderr, "rga present: %s\n", why.c_str());
    lost_ = true;
    return Fit::Lost;
  }

  bool present(const u32* const fb[2], const View* views, int nviews, const Params& p) override {
    if (p.rot != 0) return false;
    u32* px = sink_.begin_frame();
    if (!px) return false;
    const int buf = sink_.current();
    ScanoutOut::DmabufPlane plane;
    if (buf < 0 || !sink_.dmabuf_plane(buf, plane)) { sink_.end_frame(); return false; }
    // DS_RGA_DUMP=N:FILE: the finished panel buffer of the N-th presented frame, raw XRGB rows, and the views (diagnostic).
    static const char* dump = std::getenv("DS_RGA_DUMP");
    static const u64 dump_at = dump && std::strchr(dump, ':') ? std::strtoull(dump, nullptr, 10) : 300;
    // Each view's jobs: the exact path where it applies, else one scaling job.
    Plan plan[frontend::SCREENS];
    bool want_src[frontend::SCREENS] = {};
    u64 exact_sig = 0;
    for (int i = 0; i < nviews && i < frontend::SCREENS; ++i) {
      const View& v = views[i];
      Plan& pl = plan[i];
      if (!v.shown || v.rect.w <= 0 || v.rect.h <= 0) continue;
      const u32 k = p.linear ? 0 : exact_scale(v.rect);
      if (k && plan_exact(i, v.rect, k, plane, pl)) { exact_sig = exact_sig * 31 + k; continue; }
      // Clip the view to the buffer (the layout can hang over on an odd window).
      frontend::Rect r = v.rect;
      int sx = 0, sy = 0, sw = static_cast<int>(ds::SCREEN_W), sh = static_cast<int>(ds::SCREEN_H);
      if (r.x < 0) { sx = -r.x * sw / r.w; sw -= sx; r.w += r.x; r.x = 0; }
      if (r.y < 0) { sy = -r.y * sh / r.h; sh -= sy; r.h += r.y; r.y = 0; }
      if (r.x + r.w > static_cast<int>(plane.width)) { const int over = r.x + r.w - static_cast<int>(plane.width); sw -= over * sw / r.w; r.w -= over; }
      if (r.y + r.h > static_cast<int>(plane.height)) { const int over = r.y + r.h - static_cast<int>(plane.height); sh -= over * sh / r.h; r.h -= over; }
      if (r.w <= 0 || r.h <= 0 || sw <= 0 || sh <= 0) continue;
      pl.on = true; pl.k = 0; pl.r = r; pl.n = 1;
      pl.job[0] = Job{frontend::Rect{sx, sy, sw, sh}, r};
      want_src[v.screen] = true;
      exact_sig = exact_sig * 31 + (k ? 100 : 0);   // 100: wanted exact, could not place it
    }
    if (exact_sig != exact_sig_) {
      exact_sig_ = exact_sig;
      if (std::getenv("DS_VERBOSE"))
        for (int i = 0; i < nviews && i < frontend::SCREENS; ++i) if (plan[i].on)
          std::fprintf(stderr, "rga present: view %d %s\n", i, plan[i].k ? ("exact " + std::to_string(plan[i].k) + "x (column strip + " + std::to_string(plan[i].n) + " row jobs)").c_str() : "bilinear");
    }
    // The 1x frames the scaling jobs read (CPU write, cache cleaned for the device).
    const Src& s = src_[frame_ % kSlots];
    for (int k = 0; k < frontend::SCREENS; ++k) {
      if (!want_src[k]) continue;
      dmaheap::sync_begin_write(s.fd[k]);
      std::memcpy(s.px[k], fb[k], kFrameBytes);
      dmaheap::sync_end_write(s.fd[k]);
    }
    // Letterbox: black once per buffer per layout (a layout change re-blacks every buffer on its next use).
    u64 sig = static_cast<u64>(nviews);
    for (int i = 0; i < nviews && i < frontend::SCREENS; ++i) { const View& v = views[i]; sig = sig * 1000003u + static_cast<u64>(v.shown ? 1 : 0) * 7 + static_cast<u64>(v.rect.x) * 31 + static_cast<u64>(v.rect.y) * 131 + static_cast<u64>(v.rect.w) * 1031 + static_cast<u64>(v.rect.h) * 8191 + static_cast<u64>(v.screen); }
    if (sig != layout_sig_) { layout_sig_ = sig; std::fill(cleared_.begin(), cleared_.end(), false); }
    if (buf < static_cast<int>(cleared_.size()) && !cleared_[static_cast<size_t>(buf)]) {
      dmaheap::sync_begin_write(plane.fd);
      std::memset(px, 0, static_cast<size_t>(plane.stride_bytes) * plane.height);
      dmaheap::sync_end_write(plane.fd);
      cleared_[static_cast<size_t>(buf)] = true;
    }
    // The exact views' strips: the first now, each next one while the RGA
    // runs the last job of the view before it.
    bool written[frontend::SCREENS] = {};
    auto write_strip_after = [&](int after) {
      for (int i = after + 1; i < nviews && i < frontend::SCREENS; ++i) {
        if (!plan[i].on || !plan[i].k) continue;
        if (!written[i]) {
          const Strip& st = strip_[i];
          dmaheap::sync_begin_write(st.fd);
          repeat_columns(fb[views[i].screen], st.px, plan[i].k);
          dmaheap::sync_end_write(st.fd);
          written[i] = true;
        }
        return;
      }
    };
    write_strip_after(-1);
    bool shown = false;
    for (int i = 0; i < nviews && i < frontend::SCREENS; ++i) {
      const View& v = views[i];
      const Plan& pl = plan[i];
      if (!pl.on) continue;
      const frontend::Rect& r = pl.r;
      const bool fade = v.blends && p.inset_alpha < 255;
      if (fade && !scratch_ready(plane)) { std::fprintf(stderr, "rga present: no scratch buffer for the fading inset (%s); drawn opaque\n", std::strerror(errno)); }
      const bool via_scratch = fade && scratch_fd_ >= 0;
      if (dump && frame_ == dump_at) std::fprintf(stderr, "rga present: view %d screen %d rect %d,%d %dx%d shown %d blends %d alpha %u fade %d scratch %d exact %u\n", i, v.screen, v.rect.x, v.rect.y, v.rect.w, v.rect.h, v.shown, v.blends, p.inset_alpha, fade, via_scratch, pl.k);
      const int dfd = via_scratch ? scratch_fd_ : plane.fd;
      const int jfd = pl.k ? rows_[pl.k].fd : fd_;
      const int sfd = pl.k ? strip_[i].fd : s.fd[v.screen];
      const u32 sbytes = pl.k ? rows_[pl.k].strip_bytes : src_bytes_, dbytes = pl.k ? rows_[pl.k].dst_bytes : dst_bytes_;
      for (int j = 0; j < pl.n; ++j) {
        if (!submit(jfd, sfd, sbytes, pl.job[j].crop, dfd, dbytes, pl.job[j].compose)) { lost_ = true; sink_.end_frame(); return false; }
        if (j == pl.n - 1) write_strip_after(i);
        if (!finish(jfd)) { lost_ = true; sink_.end_frame(); return false; }
      }
      if (via_scratch) {
        // The inset over what is there, at its alpha (a fade timer on the PiP).
        dmaheap::sync_begin_write(plane.fd);
        const u32 pitch = plane.stride_bytes / 4, a = p.inset_alpha, ia = 255 - a;
        for (int y = r.y; y < r.y + r.h; ++y) {
          u32* d = px + static_cast<size_t>(y) * pitch;
          const u32* sp = scratch_px_ + static_cast<size_t>(y) * pitch;
          for (int x = r.x; x < r.x + r.w; ++x) {
            const u32 sv = sp[x], dv = d[x];
            const u32 rb = ((sv & 0xFF00FF) * a + (dv & 0xFF00FF) * ia) >> 8, g = ((sv & 0xFF00) * a + (dv & 0xFF00) * ia) >> 8;
            d[x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0xFF00);
          }
        }
        dmaheap::sync_end_write(plane.fd);
      }
      if (v.grid && p.grid < 256) grid_view(px, plane, r, v.rect, p.grid);
      shown = true;
    }
    // The overlay's rectangle, blended by the CPU (0xAARRGGBB over XRGB).
    Over& o = over_[frame_ % kSlots];
    if (p.drawn.w > 0 && p.drawn.h > 0 && o.w == p.lw && o.h == p.lh && p.lw <= static_cast<int>(plane.width) && p.lh <= static_cast<int>(plane.height)) {
      const int x0 = std::max(0, p.drawn.x), y0 = std::max(0, p.drawn.y), x1 = std::min(p.lw, p.drawn.x + p.drawn.w), y1 = std::min(p.lh, p.drawn.y + p.drawn.h);
      if (x1 > x0 && y1 > y0) {
        dmaheap::sync_begin_write(plane.fd);
        const u32 pitch = plane.stride_bytes / 4;
        for (int y = y0; y < y1; ++y) {
          u32* d = px + static_cast<size_t>(y) * pitch;
          const u32* srow = o.px.data() + static_cast<size_t>(y) * p.lw;
          for (int x = x0; x < x1; ++x) {
            const u32 sv = srow[x], a = sv >> 24;
            if (!a) continue;
            if (a == 255) { d[x] = sv | 0xFF000000u; continue; }
            const u32 dv = d[x];
            const u32 rb = ((sv & 0xFF00FF) * a + (dv & 0xFF00FF) * (255 - a)) >> 8, g = ((sv & 0xFF00) * a + (dv & 0xFF00) * (255 - a)) >> 8;
            d[x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0xFF00);
          }
        }
        dmaheap::sync_end_write(plane.fd);
      }
      o.dirty = p.drawn;   // cleared before this slot's next use
    } else o.dirty = frontend::Rect{};
    if (dump && frame_ == dump_at) {
      const char* path = std::strchr(dump, ':') ? std::strchr(dump, ':') + 1 : dump;
      if (std::FILE* f = std::fopen(path, "wb")) { std::fwrite(px, 1, static_cast<size_t>(plane.stride_bytes) * plane.height, f); std::fclose(f); std::fprintf(stderr, "rga present: wrote %s (%ux%u, pitch %u)\n", path, plane.width, plane.height, plane.stride_bytes / 4); }
    }
    // The jobs are synchronous, so the buffer is complete: to the sink now.
    sink_.end_frame();
    ++frame_;
    return shown;
  }

  u32* overlay(int lw, int lh) override {
    Over& o = over_[frame_ % kSlots];
    if (o.w != lw || o.h != lh) {
      for (Over& e : over_) { e.px.assign(static_cast<size_t>(lw) * lh, 0u); e.w = lw; e.h = lh; e.dirty = frontend::Rect{}; }
    } else if (o.dirty.w > 0 && o.dirty.h > 0) {
      const int x0 = std::max(0, o.dirty.x), y0 = std::max(0, o.dirty.y), x1 = std::min(lw, o.dirty.x + o.dirty.w), y1 = std::min(lh, o.dirty.y + o.dirty.h);
      for (int y = y0; y < y1; ++y) std::memset(o.px.data() + static_cast<size_t>(y) * lw + x0, 0, static_cast<size_t>(std::max(0, x1 - x0)) * 4);
      o.dirty = frontend::Rect{};
    }
    return o.px.data();
  }
  void flush() override { sink_.flush(); }

private:
  explicit RgaPresenter(ScanoutOut& sink) : sink_(sink) {}

  struct Src { int fd[frontend::SCREENS] = {-1, -1}; u32* px[frontend::SCREENS] = {}; };
  // One job: a rectangle of the source into a rectangle of the destination.
  struct Job { frontend::Rect crop, compose; };
  // A view's jobs this frame. k = 0: one scaling job from the 1x frame;
  // k > 0: the exact path, a job per band from the view's strip.
  struct Plan { bool on = false; u32 k = 0; frontend::Rect r; Job job[kMaxK]; int n = 0; };
  // The exact path's context for one scale: output 256k x 192 (the strip),
  // capture the panel described as k*W x H/k. Opened on first use.
  struct RowCtx { int fd = -1; u32 strip_bytes = 0, dst_bytes = 0; bool failed = false; };
  struct Strip { int fd = -1; u32* px = nullptr; size_t bytes = 0; };

  // k if the view is exactly k x 256 by k x 192, else 0.
  static u32 exact_scale(const frontend::Rect& r) {
    if (r.w <= 0 || r.w % static_cast<int>(ds::SCREEN_W) || r.h % static_cast<int>(ds::SCREEN_H)) return 0;
    const u32 k = static_cast<u32>(r.w) / ds::SCREEN_W;
    return k == static_cast<u32>(r.h) / ds::SCREEN_H && k <= kMaxK ? k : 0;
  }

  // The exact path's jobs for view `i` at `k`x, clipped to the buffer. Band
  // j holds the strip's rows s at panel rows y0 + k*s + j; in the k*W x H/k
  // description that is row (y0 + j + k*s) / k at x offset ((y0 + j) mod k) * W.
  // False (the view goes bilinear) where the description cannot reach it:
  // a slice under the driver's 34 px minimum, rows past the last whole
  // multiple of k, or a context or strip that will not open.
  bool plan_exact(int i, const frontend::Rect& v, u32 k, const ScanoutOut::DmabufPlane& plane, Plan& pl) {
    const int W = static_cast<int>(plane.width), H = static_cast<int>(plane.height), kk = static_cast<int>(k);
    const int he = H / kk * kk;   // rows the description reaches
    if (k * plane.width > 8192 || H / kk < kMinSide) return false;
    const int cx0 = std::max(0, v.x), cx1 = std::min(W, v.x + v.w), cy0 = std::max(0, v.y), cy1 = std::min(H, v.y + v.h);
    if (cx1 - cx0 < kMinSide || cy1 <= cy0 || cy1 > he) return false;
    int n = 0;
    for (int j = 0; j < kk; ++j) {
      const int lo = v.y + j;   // panel row of the strip's row 0 in this band
      const int s0 = std::max(0, floor_div(-lo + kk - 1, kk)), s1 = std::min(static_cast<int>(ds::SCREEN_H) - 1, floor_div(he - 1 - lo, kk));
      if (s1 < s0) continue;
      if (s1 - s0 + 1 < kMinSide) return false;
      const int y = lo + kk * s0;
      pl.job[n++] = Job{frontend::Rect{cx0 - v.x, s0, cx1 - cx0, s1 - s0 + 1}, frontend::Rect{(y % kk) * W + cx0, y / kk, cx1 - cx0, s1 - s0 + 1}};
    }
    if (!n || !rows_ready(k, plane) || !strip_ready(i, rows_[k].strip_bytes)) return false;
    pl.on = true; pl.k = k; pl.n = n;
    pl.r = frontend::Rect{cx0, cy0, cx1 - cx0, cy1 - cy0};
    return true;
  }

  bool rows_ready(u32 k, const ScanoutOut::DmabufPlane& plane) {
    RowCtx& c = rows_[k];
    if (c.fd >= 0) return true;
    if (c.failed) return false;
    c.fd = ::open(dev_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    u32 stride = 0, cstride = 0;
    const bool ok = c.fd >= 0 &&
      v4l_format(c.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, ds::SCREEN_W * k, ds::SCREEN_H, &stride, &c.strip_bytes) && stride == ds::SCREEN_W * k * 4 &&
      v4l_format(c.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, plane.width * k, plane.height / k, &cstride, &c.dst_bytes) && cstride == plane.stride_bytes * k &&
      v4l_reqbufs(c.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1) && v4l_reqbufs(c.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1) &&
      v4l_stream(c.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true) && v4l_stream(c.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true);
    if (ok) return true;
    std::fprintf(stderr, "rga present: no exact %ux path on %ux%u (%s); that scale stays bilinear\n", k, plane.width, plane.height, std::strerror(errno));
    if (c.fd >= 0) close(c.fd);
    c.fd = -1; c.failed = true;
    return false;
  }
  void close_rows() {
    for (RowCtx& c : rows_) { if (c.fd >= 0) { v4l_stream(c.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, false); v4l_stream(c.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false); close(c.fd); } c = RowCtx{}; }
  }
  // View i's strip, at least `bytes` (kept across frames; a larger scale reallocates).
  bool strip_ready(int i, size_t bytes) {
    Strip& s = strip_[i];
    if (s.fd >= 0 && s.bytes >= bytes) return true;
    if (s.px) munmap(s.px, s.bytes);
    if (s.fd >= 0) close(s.fd);
    s = Strip{};
    s.fd = dmaheap::alloc(bytes, [](int) { return true; }, "rga");
    if (s.fd < 0) return false;
    void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
    if (m == MAP_FAILED) { close(s.fd); s.fd = -1; return false; }
    s.px = static_cast<u32*>(m); s.bytes = bytes;
    return true;
  }

  // A panel-sized buffer the RGA scales a fading inset into (allocated on first need).
  bool scratch_ready(const ScanoutOut::DmabufPlane& plane) {
    const size_t need = static_cast<size_t>(plane.stride_bytes) * plane.height;
    if (scratch_fd_ >= 0 && scratch_bytes_ >= need) return true;
    if (scratch_fd_ >= 0) { munmap(scratch_px_, scratch_bytes_); close(scratch_fd_); scratch_fd_ = -1; scratch_px_ = nullptr; }
    scratch_fd_ = dmaheap::alloc(need, [](int) { return true; }, "rga");
    if (scratch_fd_ < 0) return false;
    scratch_px_ = static_cast<u32*>(mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_SHARED, scratch_fd_, 0));
    if (scratch_px_ == MAP_FAILED) { scratch_px_ = nullptr; close(scratch_fd_); scratch_fd_ = -1; return false; }
    scratch_bytes_ = need;
    return true;
  }

  // The LCD grid over one view: the first panel column / row of each source
  // pixel / line darkened to gf/256, for runs at least ceil(scale) wide,
  // every source pixel (or every other at exactly 2x) -- the scanline
  // scaler's placement (kern::scale_row_grid). `r` is the clipped rect on
  // the buffer, `full` the view's rect the runs are laid out in. One sweep
  // over the rect with a per-column factor table (NEON where built): the
  // buffer is read and written once, which is what the pass costs.
  void grid_view(u32* px, const ScanoutOut::DmabufPlane& plane, const frontend::Rect& r, const frontend::Rect& full, u32 gf) {
    const int dim[2] = {full.w, full.h}, srcn[2] = {static_cast<int>(ds::SCREEN_W), static_cast<int>(ds::SCREEN_H)};
    std::vector<u8>* marks[2] = {&grid_col_, &grid_row_};
    for (int axis = 0; axis < 2; ++axis) {
      const u32 n = static_cast<u32>(std::max(0, dim[axis])), sn = static_cast<u32>(srcn[axis]);
      std::vector<u8>& m = *marks[axis];
      m.assign(n, 0);
      if (n == 0 || n > 4096) continue;
      const u32 min_run = std::max<u32>(2, (n + sn - 1) / sn), pitch = n == 2 * sn ? 2 : 1;
      for (u32 s = 0; s < sn; ++s) {
        const u32 x0 = (s * n + sn - 1) / sn, x1 = ((s + 1) * n + sn - 1) / sn;
        if (x1 - x0 >= min_run && s % pitch == 0 && x0 < n) m[x0] = 1;
      }
    }
    // Per buffer column of the clipped rect: the factor (256 = untouched), 16-bit for the vector multiply.
    grid_fac_.assign(static_cast<size_t>(r.w) + 8, 256);
    for (int x = 0; x < r.w; ++x) { const int lx = r.x + x - full.x; if (lx >= 0 && lx < static_cast<int>(grid_col_.size()) && grid_col_[static_cast<size_t>(lx)]) grid_fac_[static_cast<size_t>(x)] = static_cast<u16>(gf); }
    dmaheap::sync_begin_write(plane.fd);
    const u32 pitch = plane.stride_bytes / 4;
    for (int y = r.y; y < r.y + r.h; ++y) {
      u32* d = px + static_cast<size_t>(y) * pitch + r.x;
      const int ly = y - full.y;
      const bool seam_row = ly >= 0 && ly < static_cast<int>(grid_row_.size()) && grid_row_[static_cast<size_t>(ly)];
      const u16* fac = grid_fac_.data();
      int x = 0;
#if DSPERATE_NEON
      const uint16x8_t gfv = vdupq_n_u16(static_cast<u16>(gf));
      for (; x + 8 <= r.w; x += 8) {
        // Two 4-pixel vectors; per-lane factors: the row's, or the column table's.
        uint8x16_t p0 = vld1q_u8(reinterpret_cast<const u8*>(d + x)), p1 = vld1q_u8(reinterpret_cast<const u8*>(d + x + 4));
        uint16x8_t f8 = seam_row ? gfv : vld1q_u16(fac + x);
        // Factor per pixel -> per byte (B G R X): widen each pixel's factor to its 4 bytes.
        uint16x4_t fa = vget_low_u16(f8), fb = vget_high_u16(f8);
        uint16x8_t f0lo = vcombine_u16(vdup_lane_u16(fa, 0), vdup_lane_u16(fa, 1)), f0hi = vcombine_u16(vdup_lane_u16(fa, 2), vdup_lane_u16(fa, 3));
        uint16x8_t f1lo = vcombine_u16(vdup_lane_u16(fb, 0), vdup_lane_u16(fb, 1)), f1hi = vcombine_u16(vdup_lane_u16(fb, 2), vdup_lane_u16(fb, 3));
        uint16x8_t m0lo = vshrq_n_u16(vmulq_u16(vmovl_u8(vget_low_u8(p0)), f0lo), 8), m0hi = vshrq_n_u16(vmulq_u16(vmovl_u8(vget_high_u8(p0)), f0hi), 8);
        uint16x8_t m1lo = vshrq_n_u16(vmulq_u16(vmovl_u8(vget_low_u8(p1)), f1lo), 8), m1hi = vshrq_n_u16(vmulq_u16(vmovl_u8(vget_high_u8(p1)), f1hi), 8);
        uint8x16_t o0 = vcombine_u8(vmovn_u16(m0lo), vmovn_u16(m0hi)), o1 = vcombine_u8(vmovn_u16(m1lo), vmovn_u16(m1hi));
        const uint8x16_t xmask = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000u));
        vst1q_u8(reinterpret_cast<u8*>(d + x), vorrq_u8(o0, xmask)); vst1q_u8(reinterpret_cast<u8*>(d + x + 4), vorrq_u8(o1, xmask));
      }
#endif
      for (; x < r.w; ++x) {
        const u32 f = seam_row ? gf : fac[x];
        if (f == 256) continue;
        const u32 v = d[x];
        d[x] = 0xFF000000u | ((((v & 0xFF00FF) * f) >> 8) & 0xFF00FF) | ((((v & 0xFF00) * f) >> 8) & 0xFF00);
      }
    }
    dmaheap::sync_end_write(plane.fd);
  }
  struct Over { std::vector<u32> px; int w = 0, h = 0; frontend::Rect dirty; };

  bool init(std::string* why) {
    fd_ = open_rga(why, &dev_);
    if (fd_ < 0) return false;
    // Source: 256x192 XRGB8888. V4L2 names it by byte order (B G R X): XR24 is
    // what the driver lists; the probe confirmed the channels land right.
    if (!set_format(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, ds::SCREEN_W, ds::SCREEN_H, why)) return false;
    if (!v4l_reqbufs(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1)) { if (why) *why = "RGA refuses dma-buf output buffers"; return false; }
    if (!v4l_stream(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true)) { if (why) *why = "RGA output stream on failed"; return false; }
    if (!setup_capture(why)) return false;
    for (Src& s : src_) for (int k = 0; k < frontend::SCREENS; ++k) {
      s.fd[k] = dmaheap::alloc(kFrameBytes, [](int) { return true; }, "rga");
      if (s.fd[k] < 0) { if (why) *why = "no dma-buf for the RGA source"; return false; }
      s.px[k] = static_cast<u32*>(mmap(nullptr, kFrameBytes, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd[k], 0));
      if (s.px[k] == MAP_FAILED) { s.px[k] = nullptr; if (why) *why = "cannot map the RGA source buffer"; return false; }
    }
    cleared_.assign(static_cast<size_t>(std::max(0, sink_.bufs())), false);
    sink_.set_gpu_writes(true);   // the device writes the sink's buffers; the CPU syncs its own touches
    name_ = std::string("RGA present (rockchip-rga, ") + (std::strcmp(SDL_GetCurrentVideoDriver(), "KMSDRM") == 0 ? "kms" : "dmabuf") + " scanout)";
    return true;
  }

  bool set_format(u32 type, u32 w, u32 h, std::string* why) {
    u32 stride = 0, bytes = 0;
    if (!v4l_format(fd_, type, w, h, &stride, &bytes)) {
      if (why) *why = std::string("RGA refuses the ") + std::to_string(w) + "x" + std::to_string(h) + " frame format: " + std::strerror(errno);
      return false;
    }
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) src_bytes_ = bytes;
    else { dst_bytes_ = bytes; dst_stride_ = stride; }
    return true;
  }
  // The capture side follows the sink's buffer: a format change is refused
  // while the queue holds buffers, so it is torn down and set up again.
  bool setup_capture(std::string* why) {
    v4l_stream(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, false);
    v4l_reqbufs(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
    if (!set_capture_format(why)) return false;
    if (!v4l_reqbufs(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1)) { if (why) *why = "RGA refuses dma-buf capture buffers"; return false; }
    if (!v4l_stream(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true)) { if (why) *why = "RGA capture stream on failed"; return false; }
    return true;
  }
  bool set_capture_format(std::string* why) {
    ScanoutOut::DmabufPlane p;
    if (!sink_.dmabuf_plane(0, p)) { if (why) *why = "the sink has no dma-buf"; return false; }
    if (p.fourcc != 0x34325258u /* XR24 */ ) { if (why) *why = "the sink's format is not XRGB8888"; return false; }
    if (!set_format(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, p.width, p.height, why)) return false;
    if (dst_stride_ != p.stride_bytes) { if (why) *why = "RGA pitch differs from the scanout pitch"; return false; }
    return true;
  }
  bool stream(bool on) { return v4l_stream(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, on) & v4l_stream(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, on); }
  // A job in two halves, so the CPU can work while the RGA runs it: source
  // rectangle `crop` of `sfd` into `compose` of `dfd` on context `fd`.
  bool submit(int fd, int sfd, u32 sbytes, const frontend::Rect& crop, int dfd, u32 dbytes, const frontend::Rect& compose) {
    // The driver checks each rectangle against the other's current size
    // (at most 16x apart), so a big change of both can need compose first.
    if (!v4l_select(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, crop)) {
      if (!v4l_select(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, compose) ||
          !v4l_select(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_SEL_TGT_CROP, crop)) return fail("crop");
    } else if (!v4l_select(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_SEL_TGT_COMPOSE, compose)) return fail("compose");
    if (!v4l_queue(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, sfd, sbytes) || !v4l_queue(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, dfd, dbytes)) return fail("queue");
    return true;
  }
  bool finish(int fd) {
    pollfd p{fd, POLLIN | POLLOUT, 0};
    if (poll(&p, 1, 500) <= 0) return fail("job timed out");
    if (!v4l_dequeue(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) || !v4l_dequeue(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) return fail("dequeue");
    return true;
  }
  bool fail(const char* what) { std::fprintf(stderr, "rga present: %s failed: %s\n", what, std::strerror(errno)); return false; }
  ScanoutOut& sink_;
  int fd_ = -1;
  std::string dev_;             // the node fd_ came from; the exact path's contexts open it too
  u32 src_bytes_ = 0, dst_bytes_ = 0, dst_stride_ = 0;
  Src src_[kSlots];
  RowCtx rows_[kMaxK + 1];      // by scale
  Strip strip_[frontend::SCREENS];   // by view
  u64 exact_sig_ = ~0ull;       // which views went exact, for the log
  int scratch_fd_ = -1; u32* scratch_px_ = nullptr; size_t scratch_bytes_ = 0;
  std::vector<u8> grid_col_, grid_row_;
  std::vector<u16> grid_fac_;
  Over over_[kSlots];
  std::vector<bool> cleared_;   // per sink buffer: letterbox blacked for this layout
  u64 frame_ = 0, layout_sig_ = 0;
  bool lost_ = false;
};

} // namespace

std::unique_ptr<FramePresenter> open_rga_presenter(ScanoutOut& sink, std::string* why) { return RgaPresenter::open(sink, why); }
#else
std::unique_ptr<FramePresenter> open_rga_presenter(ScanoutOut&, std::string* why) { if (why) *why = "RGA present needs Linux V4L2 with dma-buf mem2mem"; return nullptr; }
#endif

} // namespace ds::sdl
