// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Dmabuf presentation onto SDL's own Wayland window.
//
// The scanline-scaling path writes panel-sized frames; SDL's window surface
// then costs a shm copy on commit, and the compositor a texture upload when
// it composites us. This backend removes both: frames are rendered straight
// into CMA dma-heap buffers (physically contiguous, scannable) submitted via
// zwp_linux_dmabuf_v1. The compositor samples them zero-copy when it
// composites -- and when the surface is fullscreen, opaque and untransformed,
// wlroots lifts the buffer onto a hardware plane and its per-frame work
// drops to a flip (verified on-device: both RG DS boards' panels, VOP2 plane
// state showing our fb). We never choose between those tiers; the compositor
// does, per frame.
//
// SDL keeps everything else: the window, xdg-shell, fullscreen handling and
// input. We fetch its wl_display/wl_surface via SDL_SysWMinfo and only take
// over what gets attached. Our globals live on a private event queue so our
// dispatching and SDL's event pump never touch each other's handlers.
//
// libwayland is dlopen'd (wl_dyn.h): on a device without it, or without a
// compositor, open() fails cleanly and Display falls back to the SDL paths.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <cstddef>

struct SDL_Window;
struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wl_registry;
struct wl_compositor;
struct wl_buffer;
struct wl_output;
struct zwp_linux_dmabuf_v1;
struct xdg_toplevel;

namespace ds::sdl {

class DmabufOut : public ScanoutOut {
public:
  static constexpr int BUFS = 3;   // one on screen, one queued, one being drawn

  // False if any precondition is missing (no libwayland, not the wayland
  // video driver, no dmabuf global, CMA allocation failed); the caller logs
  // and uses another path. w/h is the buffer size in pixels.
  //
  // `output_index` >= 0 asks the compositor to fullscreen this window on
  // that output (registry advertisement order). SDL cannot: Wayland has no
  // client-side window positions, so SDL's display-index parameters are
  // no-ops there and the request must go through the window's own
  // xdg_toplevel. Without it a dual-window layout lands wherever the
  // compositor pleases -- and a mis-placed surface can never scan out.
  bool open(SDL_Window* win, int w, int h, int output_index = -1);

  bool reopen(SDL_Window* win, int w, int h) override { const int o = output_index_; close(); return open(win, w, h, o); }
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int bufs() const override { return BUFS; }
  int current() const override { return cur_; }

  // Pixels of a free buffer to render the next frame into (blocks on the
  // compositor if all are pending, which is the vsync). Null on protocol
  // error; the caller falls back.
  u32* begin_frame() override;
  void end_frame() override;       // attach + damage + commit + flush

  // Public for the C listener table; not part of the interface.
  static void on_release(void* data, struct wl_buffer* wb);

private:
  struct Buf {
    int fd = -1;
    u32* px = nullptr;
    size_t bytes = 0;
    struct wl_buffer* wb = nullptr;
    bool busy = false;
  };
  bool alloc_buf(Buf& b);
  void drop_buf(Buf& b);

  struct wl_display* dpy_ = nullptr;      // SDL's; not ours to destroy
  struct wl_surface* surf_ = nullptr;     // SDL's; not ours to destroy
  struct wl_event_queue* q_ = nullptr;    // the process-wide globals' queue
  Buf bufs_[BUFS];
  int cur_ = -1;
  int w_ = 0, h_ = 0;
  int output_index_ = -1;                 // the open() argument, for reopen()
  bool dead_ = false;                     // protocol error; stop submitting
};

} // namespace ds::sdl
