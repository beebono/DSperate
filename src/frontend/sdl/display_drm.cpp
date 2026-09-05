// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "display_drm.h"
#include "dmaheap.h"
#include "drm_uapi.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

namespace {

// Every DrmOut on the same DRM fd, so completions can be routed by owner.
std::vector<DrmOut*> g_outs;

constexpr u32 FMT_XRGB8888 = 0x34325258;   // 'XR24'; the core writes 0xAARRGGBB and we are opaque

// SDL's DRM fd. The union member is behind SDL_VIDEO_DRIVER_KMSDRM, which is
// a property of the SDL build our *headers* came from and not of the runtime
// library, so fall back to the union's byte view -- the layout
// (int dev_index; int drm_fd; struct gbm_device*) is part of SDL's ABI.
int sdl_drm_fd(SDL_Window* win) {
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(win, &wm)) { std::fprintf(stderr, "drm: SDL_GetWindowWMInfo: %s\n", SDL_GetError()); return -1; }
  if (wm.subsystem != SDL_SYSWM_KMSDRM) { std::fprintf(stderr, "drm: not a KMSDRM window\n"); return -1; }
#if defined(SDL_VIDEO_DRIVER_KMSDRM)
  return wm.info.kmsdrm.drm_fd;
#else
  int ints[2] = {0, -1};
  std::memcpy(ints, wm.info.dummy, sizeof ints);
  return ints[1];
#endif
}

} // namespace

bool DrmOut::alloc_buf(Buf& b) {
  // The import is the probe: a heap the kernel hands out but the display
  // controller cannot scan (system memory without an IOMMU) fails ADDFB2,
  // and dmaheap moves on to the next one.
  const size_t bytes = static_cast<size_t>(w_) * h_ * 4;
  b.fd = dmaheap::alloc(bytes, [&](int fd) {
    drmu::prime_handle ph = {};
    ph.fd = fd;
    if (ioctl(fd_, drmu::IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0) { std::perror("drm: PRIME_FD_TO_HANDLE"); return false; }
    drmu::mode_fb_cmd2 f = {};
    f.width = static_cast<u32>(w_);
    f.height = static_cast<u32>(h_);
    f.pixel_format = FMT_XRGB8888;
    f.handles[0] = ph.handle;
    f.pitches[0] = static_cast<u32>(w_) * 4;
    if (ioctl(fd_, drmu::IOCTL_MODE_ADDFB2, &f) < 0) {
      std::perror("drm: ADDFB2");
      drmu::gem_close gc = {}; gc.handle = ph.handle; ioctl(fd_, drmu::IOCTL_GEM_CLOSE, &gc);
      return false;
    }
    b.handle = ph.handle;
    b.fb = f.fb_id;
    return true;
  }, "drm");
  if (b.fd < 0) return false;
  b.bytes = bytes;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (m == MAP_FAILED) { std::perror("drm: mmap"); return false; }
  b.px = static_cast<u32*>(m);
  std::memset(b.px, 0, b.bytes);
  return true;
}

void DrmOut::drop_buf(Buf& b) {
  if (b.fb) { unsigned int id = b.fb; ioctl(fd_, drmu::IOCTL_MODE_RMFB, &id); }
  if (b.handle) { drmu::gem_close gc = {}; gc.handle = b.handle; ioctl(fd_, drmu::IOCTL_GEM_CLOSE, &gc); }
  if (b.px) munmap(b.px, b.bytes);
  if (b.fd >= 0) ::close(b.fd);
  b = Buf{};
}

bool DrmOut::open(SDL_Window* win, int w, int h, int display_index) {
  fd_ = sdl_drm_fd(win);
  if (fd_ < 0) return false;
  display_ = display_index;
  w_ = w; h_ = h;

  // Resources, then the connectors, in the order SDL enumerated its displays.
  drmu::mode_card_res res = {};
  if (ioctl(fd_, drmu::IOCTL_MODE_GETRESOURCES, &res) < 0) { std::perror("drm: GETRESOURCES"); return false; }
  std::vector<u32> conns(res.count_connectors), crtcs(res.count_crtcs), encs(res.count_encoders);
  res.connector_id_ptr = reinterpret_cast<uint64_t>(conns.data());
  res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
  res.encoder_id_ptr = reinterpret_cast<uint64_t>(encs.data());
  res.count_fbs = 0; res.fb_id_ptr = 0;
  if (ioctl(fd_, drmu::IOCTL_MODE_GETRESOURCES, &res) < 0) { std::perror("drm: GETRESOURCES"); return false; }

  drmu::mode_modeinfo mode = {};
  bool found = false;
  int seen = 0;
  for (u32 c : conns) {
    drmu::mode_get_connector gc = {};
    gc.connector_id = c;
    if (ioctl(fd_, drmu::IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
    std::vector<drmu::mode_modeinfo> modes(gc.count_modes);
    std::vector<u32> cencs(gc.count_encoders);
    gc.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
    gc.encoders_ptr = reinterpret_cast<uint64_t>(cencs.data());
    gc.count_props = 0; gc.props_ptr = 0; gc.prop_values_ptr = 0;
    if (ioctl(fd_, drmu::IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
    if (gc.connection != drmu::CONNECTOR_CONNECTED || gc.count_modes == 0) continue;
    if (seen++ != display_index) continue;

    conn_ = c;
    // The mode must be the one the window is the size of: a page flip's
    // framebuffer has to cover the CRTC, so a windowed (sub-panel) window
    // cannot use this tier at all.
    for (const drmu::mode_modeinfo& m : modes)
      if (m.hdisplay == w && m.vdisplay == h) { mode = m; found = true; break; }
    if (!found) {
      std::fprintf(stderr, "drm: display %d has no %dx%d mode (panel is %ux%u); scanout needs a fullscreen window\n",
                   display_index, w, h, modes[0].hdisplay, modes[0].vdisplay);
      return false;
    }
    // The CRTC the connector's encoder is already wired to, else any free one.
    drmu::mode_get_encoder ge = {};
    ge.encoder_id = gc.encoder_id ? gc.encoder_id : (gc.count_encoders ? cencs[0] : 0);
    if (ge.encoder_id && ioctl(fd_, drmu::IOCTL_MODE_GETENCODER, &ge) == 0 && ge.crtc_id) crtc_ = ge.crtc_id;
    if (!crtc_ && !crtcs.empty()) crtc_ = crtcs[display_index < static_cast<int>(crtcs.size()) ? display_index : 0];
    break;
  }
  if (!conn_ || !crtc_) { std::fprintf(stderr, "drm: no connector/crtc for display %d\n", display_index); return false; }

  for (Buf& b : bufs_)
    if (!alloc_buf(b)) { close(); return false; }

  // Take the CRTC over with our first buffer. A modeset rather than a flip
  // because SDL's KMSDRM only modesets on its first GL swap, which in
  // scanline mode never happens -- the CRTC may still be showing whatever
  // the console left behind.
  drmu::mode_crtc sc = {};
  sc.crtc_id = crtc_;
  sc.fb_id = bufs_[0].fb;
  sc.mode = mode;
  sc.mode_valid = 1;
  sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&conn_);
  sc.count_connectors = 1;
  if (ioctl(fd_, drmu::IOCTL_MODE_SETCRTC, &sc) < 0) { std::perror("drm: SETCRTC"); close(); return false; }
  bufs_[0].busy = true;
  on_screen_ = 0;
  pending_ = -1;
  dead_ = false;
  g_outs.push_back(this);
  std::fprintf(stderr, "drm: display %d on connector %u crtc %u, %dx%d@%u\n",
               display_index, conn_, crtc_, w_, h_, mode.vrefresh);
  return true;
}

void DrmOut::close() {
  if (fd_ >= 0) {
    // Let an outstanding flip retire before its buffer is unmapped.
    while (pending_ >= 0 && !dead_)
      if (!pump(fd_, true)) break;
    for (Buf& b : bufs_) drop_buf(b);
  }
  g_outs.erase(std::remove(g_outs.begin(), g_outs.end(), this), g_outs.end());
  fd_ = -1; crtc_ = 0; conn_ = 0;
  cur_ = on_screen_ = pending_ = -1;
  w_ = h_ = 0;
  dead_ = false;
}

void DrmOut::retire() {
  if (pending_ < 0) return;
  if (on_screen_ >= 0) bufs_[on_screen_].busy = false;
  on_screen_ = pending_;
  pending_ = -1;
}

// One read drains whatever is queued; each flip-complete carries the
// submitting DrmOut as its user_data, which is how a completion finds its
// owner on a shared fd.
bool DrmOut::pump(int fd, bool block) {
  pollfd p = {fd, POLLIN, 0};
  const int r = poll(&p, 1, block ? 1000 : 0);
  if (r < 0) return errno == EINTR;
  if (r == 0) {
    if (!block) return true;
    std::fprintf(stderr, "drm: no flip completion in 1s\n");
    return false;
  }
  alignas(8) char buf[512];
  const ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) return true;
  for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(drmu::event)) <= n;) {
    drmu::event e;
    std::memcpy(&e, buf + off, sizeof e);
    if (e.length < sizeof(drmu::event) || off + static_cast<ssize_t>(e.length) > n) break;
    if (e.type == drmu::EVENT_FLIP_COMPLETE && e.length >= sizeof(drmu::event_vblank)) {
      drmu::event_vblank v;
      std::memcpy(&v, buf + off, sizeof v);
      for (DrmOut* o : g_outs)
        if (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(o)) == v.user_data) { o->retire(); break; }
    }
    off += static_cast<ssize_t>(e.length);
  }
  return true;
}

u32* DrmOut::begin_frame() {
  if (dead_ || fd_ < 0) return nullptr;
  // Only one flip may be outstanding per CRTC, so end_frame() can only queue
  // once this one has retired. Waiting for it here is the vsync.
  pump(fd_, false);
  while (pending_ >= 0)
    if (!pump(fd_, true)) { dead_ = true; return nullptr; }
  for (int i = 0; i < BUFS; ++i)
    if (!bufs_[i].busy) { cur_ = i; return bufs_[i].px; }
  // BUFS >= 2 and at most one buffer is on screen with none pending, so this
  // is unreachable; treat it as a driver bug rather than looping forever.
  std::fprintf(stderr, "drm: no free buffer\n");
  dead_ = true;
  return nullptr;
}

void DrmOut::end_frame() {
  if (dead_ || cur_ < 0) return;
  Buf& b = bufs_[cur_];
  drmu::mode_crtc_page_flip pf = {};
  pf.crtc_id = crtc_;
  pf.fb_id = b.fb;
  pf.flags = drmu::PAGE_FLIP_EVENT;
  pf.user_data = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  if (ioctl(fd_, drmu::IOCTL_MODE_PAGE_FLIP, &pf) < 0) {
    std::perror("drm: PAGE_FLIP");
    dead_ = true;
    cur_ = -1;
    return;
  }
  b.busy = true;
  pending_ = cur_;
  cur_ = -1;
}

} // namespace ds::sdl
