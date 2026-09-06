// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Direct KMS scanout: the KMSDRM counterpart of the Wayland dmabuf tier.
//
// SDL2's KMSDRM backend implements no window framebuffer, so *every* SDL
// presentation path there is secretly the same one -- SDL_GetWindowSurface
// and the software renderer both route through SDL_CreateWindowTexture, i.e.
// a hidden GLES renderer. The default path costs a scalar stretch blit, a
// full-screen memcpy into a streaming GL texture, a textured quad, and an
// eglSwapBuffers that blocks on the pending flip: 13.4 ms per frame on the
// RG DS against 0.2 ms for the same content on Wayland dmabuf (etody, 900
// frames, both panels). Roughly 4.5 ms of that is CPU copying and the rest
// is the blocking swap; ~7 ms is the floor for any SDL path there.
//
// So we do not present through SDL at all. The scanline path already writes
// panel-sized frames; here they are written straight into CMA dma-heap
// buffers, imported as DRM framebuffers, and page-flipped onto the CRTC. The
// present becomes one non-blocking ioctl -- measured at 0.045 ms for both
// panels of the dual-panel board (tools/kms_scanout_probe.cpp).
//
// SDL keeps the window, the input and the DRM master; we borrow its DRM fd
// through SDL_SysWMinfo and take over only what is scanned out. Nothing else
// in the process flips that CRTC, because in scanline mode there is no
// renderer and no GL surface, so SDL never swaps.
//
// No libdrm and no kernel DRM headers: the ioctls go through drm_uapi.h. A
// device where any of this is missing fails open() cleanly and Display falls
// back to SDL's window surface.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <cstddef>

namespace ds::sdl {

class DrmOut : public ScanoutOut {
public:
  static constexpr int BUFS = 3;   // one on screen, one flipping, one being drawn

  // False if any precondition is missing (not the KMSDRM video driver, no
  // usable connector for this display, the window is not the size of the
  // panel's mode, CMA allocation or the DRM import failed).
  //
  // `display_index` is SDL's: KMSDRM enumerates one display per connected
  // connector in DRM resource order, so the Nth connected connector is SDL
  // display N. A dual-window layout needs this to pick its own panel.
  bool open(SDL_Window* win, int w, int h, int display_index);

  bool reopen(SDL_Window* win, int w, int h) override { const int d = display_; close(); return open(win, w, h, d); }
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int bufs() const override { return BUFS; }
  int current() const override { return cur_; }

  // Waits for the outstanding flip to retire -- that wait is the display's
  // pacing, and it is taken here rather than in end_frame() so it overlaps
  // the frame's emulation instead of extending its present.
  u32* begin_frame() override;
  void end_frame() override;      // queue the flip; does not wait

private:
  struct Buf {
    int fd = -1;                  // dma-heap buffer
    u32* px = nullptr;
    size_t bytes = 0;
    u32 handle = 0;               // GEM handle from the PRIME import
    u32 fb = 0;                   // DRM framebuffer id
    bool busy = false;            // on screen, or queued to be
  };
  bool alloc_buf(Buf& b);
  void drop_buf(Buf& b);
  // Reads whatever the DRM fd has queued and hands each completion to the
  // instance that asked for it. The fd is SDL's and a dual-window layout has
  // one DrmOut per panel on it, so an instance that consumed events by count
  // rather than by owner would retire the other panel's buffer -- a read of
  // a buffer already being drawn into. `block` waits for at least one event.
  static bool pump(int fd, bool block);
  void retire();                  // our flip completed

  int fd_ = -1;                   // SDL's DRM fd; not ours to close
  u32 crtc_ = 0, conn_ = 0;
  int display_ = 0;
  int w_ = 0, h_ = 0;
  Buf bufs_[BUFS];
  int cur_ = -1;                  // buffer handed out by begin_frame()
  int on_screen_ = -1;            // buffer the CRTC is scanning out
  int pending_ = -1;              // buffer whose flip has not completed
  bool dead_ = false;             // driver error; stop submitting
};

} // namespace ds::sdl
