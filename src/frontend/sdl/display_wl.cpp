// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "wl_dyn.h"             // must precede every wayland header
#include "display_wl.h"

#include "wl/wayland-client.h"
#include "wl/linux-dmabuf-v1.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// From <linux/dma-heap.h>, declared here so the sysroot need not carry it.
struct dma_heap_allocation_data {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

namespace ds::sdl {

namespace {

constexpr u32 FMT_XRGB8888 = 0x34325258;   // 'XR24'; the core writes 0xAARRGGBB and we are opaque

struct Globals { zwp_linux_dmabuf_v1* dmabuf; wl_compositor* comp; wl_event_queue* q; };

void on_global(void* data, wl_registry* reg, u32 name, const char* iface, u32 ver) {
  Globals* g = static_cast<Globals*>(data);
  if (!std::strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
    // v3 create_params semantics are all we use; they are unchanged in v4.
    g->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
        wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g->dmabuf), g->q);
  } else if (!std::strcmp(iface, wl_compositor_interface.name)) {
    g->comp = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 1));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g->comp), g->q);
  }
}
void on_global_remove(void*, wl_registry*, u32) {}
const wl_registry_listener reg_listener = { on_global, on_global_remove };

} // namespace

void DmabufOut::on_release(void* data, wl_buffer*) { static_cast<Buf*>(data)->busy = false; }

namespace { const wl_buffer_listener buf_listener = { DmabufOut::on_release }; }

bool DmabufOut::alloc_buf(Buf& b) {
  int heap = ::open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
  if (heap < 0) { std::perror("dmabuf: /dev/dma_heap/linux,cma"); return false; }
  dma_heap_allocation_data a = {};
  a.len = static_cast<uint64_t>(w_) * h_ * 4;
  a.fd_flags = O_RDWR | O_CLOEXEC;
  const int r = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
  ::close(heap);
  if (r < 0) { std::perror("dmabuf: CMA alloc"); return false; }
  b.fd = static_cast<int>(a.fd);
  b.bytes = a.len;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (m == MAP_FAILED) { std::perror("dmabuf: mmap"); return false; }
  b.px = static_cast<u32*>(m);
  std::memset(b.px, 0, b.bytes);

  zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(dmabuf_);
  zwp_linux_buffer_params_v1_add(p, b.fd, 0, 0, w_ * 4, 0, 0);   // plane 0, LINEAR
  b.wb = zwp_linux_buffer_params_v1_create_immed(p, w_, h_, FMT_XRGB8888, 0);
  zwp_linux_buffer_params_v1_destroy(p);
  if (!b.wb) return false;
  wl_buffer_add_listener(b.wb, &buf_listener, &b);
  return true;
}

void DmabufOut::drop_buf(Buf& b) {
  if (b.wb) wl_buffer_destroy(b.wb);
  if (b.px) munmap(b.px, b.bytes);
  if (b.fd >= 0) ::close(b.fd);
  b = Buf{};
}

bool DmabufOut::open(SDL_Window* win, int w, int h) {
  if (!wldyn::load()) { std::fprintf(stderr, "dmabuf: no libwayland (%s)\n", wldyn::error()); return false; }

  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
    std::fprintf(stderr, "dmabuf: not a wayland window\n");
    return false;
  }
  dpy_ = static_cast<wl_display*>(wm.info.wl.display);
  surf_ = static_cast<wl_surface*>(wm.info.wl.surface);
  w_ = w; h_ = h;

  q_ = wl_display_create_queue(dpy_);
  if (!q_) return false;
  reg_ = wl_display_get_registry(dpy_);
  // The registry inherits SDL's default queue; move it to ours before any
  // dispatch can deliver its events to SDL's handlers.
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(reg_), q_);
  Globals g = { nullptr, nullptr, q_ };
  wl_registry_add_listener(reg_, &reg_listener, &g);
  wl_display_roundtrip_queue(dpy_, q_);
  dmabuf_ = g.dmabuf;
  comp_ = g.comp;
  if (!dmabuf_) { std::fprintf(stderr, "dmabuf: compositor lacks zwp_linux_dmabuf_v1\n"); close(); return false; }

  for (Buf& b : bufs_)
    if (!alloc_buf(b)) { close(); return false; }
  wl_display_roundtrip_queue(dpy_, q_);   // surface any create_immed protocol error now, not mid-game

  // Fullscreen and opaque are two of the three scanout conditions (the third
  // -- an untransformed output -- is the compositor's). SDL declares opacity
  // from the window's pixel format, which has alpha, so declare it ourselves.
  if (comp_) {
    wl_region* r = wl_compositor_create_region(comp_);
    wl_region_add(r, 0, 0, w_, h_);
    wl_surface_set_opaque_region(surf_, r);
    wl_region_destroy(r);
  }
  return true;
}

void DmabufOut::close() {
  for (Buf& b : bufs_) drop_buf(b);
  if (comp_) { wl_proxy_destroy(reinterpret_cast<wl_proxy*>(comp_)); comp_ = nullptr; }
  if (dmabuf_) { wl_proxy_destroy(reinterpret_cast<wl_proxy*>(dmabuf_)); dmabuf_ = nullptr; }
  if (reg_) { wl_proxy_destroy(reinterpret_cast<wl_proxy*>(reg_)); reg_ = nullptr; }
  if (q_) { wl_event_queue_destroy(q_); q_ = nullptr; }
  dpy_ = nullptr; surf_ = nullptr; cur_ = -1; dead_ = false;
}

u32* DmabufOut::begin_frame() {
  if (dead_) return nullptr;
  for (;;) {
    wl_display_dispatch_queue_pending(dpy_, q_);
    for (int i = 0; i < BUFS; ++i)
      if (!bufs_[i].busy) { cur_ = i; return bufs_[i].px; }
    // All buffers pending: wait for a release. This is where the display's
    // pacing is felt, the same place the shm path feels its commit.
    wl_display_flush(dpy_);
    if (wl_display_dispatch_queue(dpy_, q_) < 0) {
      std::fprintf(stderr, "dmabuf: display error %d; falling back\n", wl_display_get_error(dpy_));
      dead_ = true;
      return nullptr;
    }
  }
}

void DmabufOut::end_frame() {
  if (dead_ || cur_ < 0) return;
  Buf& b = bufs_[cur_];
  b.busy = true;
  wl_surface_attach(surf_, b.wb, 0, 0);
  wl_surface_damage(surf_, 0, 0, w_, h_);
  wl_surface_commit(surf_);
  wl_display_flush(dpy_);
  cur_ = -1;
}

} // namespace ds::sdl
