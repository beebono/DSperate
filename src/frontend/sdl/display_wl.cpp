// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "wl_dyn.h"             // must precede every wayland header
#include "display_wl.h"
#include "dmaheap.h"

#include "wl/wayland-client.h"
#include "wl/linux-dmabuf-v1.h"
#include "wl/xdg-shell.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ds::sdl {

namespace {

constexpr u32 FMT_XRGB8888 = 0x34325258;   // 'XR24'; the core writes 0xAARRGGBB and we are opaque

// The globals are bound once per process and never destroyed: proxies we
// create can be referenced by events queued for OTHER dispatchers -- the
// compositor sends wl_surface.enter to SDL's queue naming *our* wl_output
// binding -- so tearing them down on a close/reopen leaves a dangling proxy
// in a queue we don't control (measured as a use-after-free inside SDL's
// dispatch on the first fullscreen configure). Four proxies and a queue for
// the lifetime of the connection is the correct price.
struct Globals {
  wl_display* dpy = nullptr;
  wl_event_queue* q = nullptr;
  wl_registry* reg = nullptr;
  zwp_linux_dmabuf_v1* dmabuf = nullptr;
  wl_compositor* comp = nullptr;
  wl_output* outputs[4] = {};
  int n_outputs = 0;
  bool tried = false;
};
Globals g_;

void on_global(void* data, wl_registry* reg, u32 name, const char* iface, u32 ver) {
  Globals* g = static_cast<Globals*>(data);
  if (!std::strcmp(iface, wl_output_interface.name)) {
    if (g->n_outputs < 4) {
      wl_output* o = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 1));
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(o), g->q);
      g->outputs[g->n_outputs++] = o;
    }
  } else if (!std::strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
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

// Bind the globals on first use; idempotent, failure sticky for the session.
bool globals_init(wl_display* dpy) {
  if (g_.tried) return g_.dpy == dpy && g_.dmabuf;
  g_.tried = true;
  g_.dpy = dpy;
  g_.q = wl_display_create_queue(dpy);
  if (!g_.q) return false;
  g_.reg = wl_display_get_registry(dpy);
  // The registry inherits SDL's default queue; move it to ours before any
  // dispatch can deliver its events to SDL's handlers.
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g_.reg), g_.q);
  wl_registry_add_listener(g_.reg, &reg_listener, &g_);
  wl_display_roundtrip_queue(dpy, g_.q);
  if (!g_.dmabuf) std::fprintf(stderr, "dmabuf: compositor lacks zwp_linux_dmabuf_v1\n");
  return g_.dmabuf != nullptr;
}

} // namespace

void DmabufOut::on_release(void* data, wl_buffer*) { static_cast<Buf*>(data)->busy = false; }

namespace { const wl_buffer_listener buf_listener = { DmabufOut::on_release }; }

namespace {
// The non-immediate create: create_immed answers a buffer the compositor
// cannot import with a fatal protocol error, which is no way to probe heaps.
struct Created { wl_buffer* wb = nullptr; bool done = false; };
void on_created(void* d, zwp_linux_buffer_params_v1*, wl_buffer* wb) { auto* c = static_cast<Created*>(d); c->wb = wb; c->done = true; }
void on_failed(void* d, zwp_linux_buffer_params_v1*) { static_cast<Created*>(d)->done = true; }
const zwp_linux_buffer_params_v1_listener params_listener = { on_created, on_failed };
} // namespace

bool DmabufOut::alloc_buf(Buf& b) {
  const size_t bytes = static_cast<size_t>(w_) * h_ * 4;
  b.fd = dmaheap::alloc(bytes, [&](int fd) {
    zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(g_.dmabuf);
    Created c;
    zwp_linux_buffer_params_v1_add_listener(p, &params_listener, &c);
    zwp_linux_buffer_params_v1_add(p, fd, 0, 0, w_ * 4, 0, 0);   // plane 0, LINEAR
    zwp_linux_buffer_params_v1_create(p, w_, h_, FMT_XRGB8888, 0);
    while (!c.done && wl_display_roundtrip_queue(dpy_, g_.q) >= 0) {}
    zwp_linux_buffer_params_v1_destroy(p);
    if (!c.wb) { std::fprintf(stderr, "dmabuf: compositor refused the buffer\n"); return false; }
    b.wb = c.wb;
    return true;
  }, "dmabuf");
  if (b.fd < 0) return false;
  b.bytes = bytes;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (m == MAP_FAILED) { std::perror("dmabuf: mmap"); return false; }
  b.px = static_cast<u32*>(m);
  std::memset(b.px, 0, b.bytes);
  wl_buffer_add_listener(b.wb, &buf_listener, &b);
  return true;
}

void DmabufOut::drop_buf(Buf& b) {
  if (b.wb) wl_buffer_destroy(b.wb);
  if (b.px) munmap(b.px, b.bytes);
  if (b.fd >= 0) ::close(b.fd);
  b = Buf{};
}

bool DmabufOut::open(SDL_Window* win, int w, int h, int output_index) {
  if (!wldyn::load()) { std::fprintf(stderr, "dmabuf: no libwayland (%s)\n", wldyn::error()); return false; }

  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
    std::fprintf(stderr, "dmabuf: not a wayland window\n");
    return false;
  }
  output_index_ = output_index;
  dpy_ = static_cast<wl_display*>(wm.info.wl.display);
  surf_ = static_cast<wl_surface*>(wm.info.wl.surface);
  w_ = w; h_ = h;
  if (!globals_init(dpy_)) { dpy_ = nullptr; surf_ = nullptr; return false; }
  q_ = g_.q;

#if SDL_VERSION_ATLEAST(2, 0, 18)
  if (output_index >= 0) {
    if (output_index >= g_.n_outputs) {
      std::fprintf(stderr, "dmabuf: output %d of %d not present\n", output_index, g_.n_outputs);
      close();
      return false;
    }
    // The toplevel is SDL's; the request only needs its proxy and our
    // wl_output bind. The compositor answers with a configure carrying the
    // output's size, which the caller's per-frame size check then follows.
    if (auto* tl = static_cast<xdg_toplevel*>(wm.info.wl.xdg_toplevel))
      xdg_toplevel_set_fullscreen(tl, g_.outputs[output_index]);
    else
      std::fprintf(stderr, "dmabuf: SDL exposes no xdg_toplevel; cannot target output %d\n", output_index);
  }
#else
  (void)output_index;
#endif

  for (Buf& b : bufs_)
    if (!alloc_buf(b)) { close(); return false; }
  wl_display_roundtrip_queue(dpy_, q_);   // surface any create_immed protocol error now, not mid-game

  // Fullscreen and opaque are two of the three scanout conditions (the third
  // -- an untransformed output -- is the compositor's). SDL declares opacity
  // from the window's pixel format, which has alpha, so declare it ourselves.
  if (g_.comp) {
    wl_region* r = wl_compositor_create_region(g_.comp);
    wl_region_add(r, 0, 0, w_, h_);
    wl_surface_set_opaque_region(surf_, r);
    wl_region_destroy(r);
  }
  return true;
}

void DmabufOut::close() {
  // Only per-instance objects die here; the globals (queue included) live for
  // the connection -- see the comment on Globals.
  for (Buf& b : bufs_) drop_buf(b);
  dpy_ = nullptr; surf_ = nullptr; q_ = nullptr; cur_ = -1; dead_ = false;
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
