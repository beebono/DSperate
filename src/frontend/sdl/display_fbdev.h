// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Direct fbdev scanout: the panel through /dev/fb0, for devices whose SDL2
// has no KMSDRM or Wayland driver at all.
//
// Why this tier exists: spruceOS runs the Anbernic H700 line (RG35XX Plus/
// H/SP, RG40XX, RG28XX, RG34XX, RGCubeXX) on BaseOS, which ships no libdrm,
// no libgbm and no Wayland. The only SDL2 that reaches those panels is a
// mali-fbdev build, and its one video driver is Mali EGL over fbdev: no
// window framebuffer, so SDL_GetWindowSurface and the software renderer
// both end in a hidden GLES upload and a blocking eglSwapBuffers -- the
// same trap display_drm.h describes for KMSDRM, and the same one the A30
// needed display_disp.h to escape.
//
// So, as on KMSDRM, we do not present through SDL. The scanline path writes
// panel-sized frames straight into fb0's memory, which is triple-buffered
// inside its virtual height, and the present is one FBIOPAN_DISPLAY. SDL is
// kept for the window (on its headless driver, so no EGL surface ever
// touches fb0 -- main.cpp arranges that before SDL_Init, exactly as for the
// display-engine tier), the events and the pad.
//
// The waiting: FBIOPAN_DISPLAY blocks until the refresh on the Allwinner
// fb drivers (measured on the A30; checked once here, with FBIO_WAITFORVSYNC
// and then the clock as fallbacks). With vsync on, the pan runs on a
// presenter thread and begin_frame() blocks only when all three buffers
// are spoken for -- displayed, latched for the next refresh, and drawn --
// which is the display's pacing, felt before the frame's emulation. There
// is no vsync-off mode: the pan itself waits for the refresh on these
// drivers, so pans always come from the thread and --no-vsync is a no-op.
//
// Fails open() cleanly where fb0 is not 32 bpp, not 0xAARRGGBB, or cannot
// hold two panel-sized buffers; Display then takes SDL's own paths.
#pragma once

#include "core/types.h"
#include "scanout.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include <linux/fb.h>

namespace ds::sdl {

class FbdevOut : public ScanoutOut {
public:
  static constexpr int MAX_BUFS = 3;

  // True when /dev/fb0 opens read-write and describes a 32 bpp panel.
  // Cheap, cached; safe to call before SDL_Init.
  static bool available();

  // Takes the panel's size from fb0 and resizes `win` to it, so the layout
  // and the buffers agree (the window lives on a headless driver and has
  // no size of its own worth keeping). False leaves nothing changed.
  bool open(SDL_Window* win, bool vsync);

  // The window's size is not the truth here, the panel's is: a configure
  // on the headless driver (a fullscreen toggle) is simply re-asserted.
  bool reopen(SDL_Window* win, int w, int h) override;
  void close() override;

  int width() const override { return w_; }
  int height() const override { return h_; }
  int stride() const override { return stride_; }
  int bufs() const override { return bufs_; }

  u32* begin_frame() override;
  void end_frame() override;

private:
  void pan(int buf);
  void wait_vsync();
  void presenter();
  void fit_window(SDL_Window* win) const;
  u32* buf_ptr(int buf) const { return reinterpret_cast<u32*>(map_ + static_cast<size_t>(buf) * buf_bytes_); }

  int  fd_ = -1;
  u8*  map_ = nullptr;
  size_t map_len_ = 0;
  size_t buf_bytes_ = 0;            // one buffer: line_length * yres
  int  w_ = 0, h_ = 0, stride_ = 0; // stride in pixels
  int  bufs_ = 0;
  fb_var_screeninfo var_{};
  bool vsync_ = true;
  bool pan_blocks_ = true;          // FBIOPAN_DISPLAY waits for the refresh (measured once)
  bool pan_measured_ = false;
  bool waitforvsync_ = true;        // FBIO_WAITFORVSYNC works (until it fails once)
  u64  next_ns_ = 0;                // clock pacing when neither waits
  int  cur_ = -1;                   // buffer handed out by begin_frame()
  std::atomic<bool> dead_{false};

  // Presenter thread state (vsync only). Buffer indices; -1 = none.
  std::thread             thread_;
  std::mutex              mu_;
  std::condition_variable cv_;
  int  displayed_ = 0;              // on the panel now
  int  latched_ = -1;               // panned to, waiting for the refresh
  int  pending_ = -1;               // drawn, not yet panned
  bool stop_ = false;
};

} // namespace ds::sdl
