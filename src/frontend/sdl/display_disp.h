// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Presentation through the Allwinner display engine's scaler layer, for the
// Miyoo A30 (Allwinner A33, "disp 1.5" driver) and anything else that
// answers the same /dev/disp ioctls.
//
// Why this tier exists: the A30's SDL2 has one video driver, Mali EGL over
// fbdev, and every SDL path to the panel is a GLES upload and swap (3 ms per
// frame with --accel, 97 ms through the window surface). The panel is also
// mounted portrait, and rotating a panel-sized frame on the Cortex-A7 costs
// 4.7 ms -- more than the GPU present it would replace. The display engine
// removes both costs at once: a layer in scaler mode takes a source of any
// size and scales it to a screen window in hardware, and the source can be
// the DS's own resolution. So the core's 256x192 framebuffers are rotated
// at DS resolution (two 48 K-pixel transposes, NEON, into an uncached buffer
// with 64-byte stores) into a composite the layer scans out scaled. No GL,
// no driver threads, no panel-sized copy. Measured on the unit: a source
// repaint 0.36 ms, the address flip 0.05 ms.
//
// What it takes over: the UI's own framebuffer layer is disabled while this
// is open (the panel is ours) and re-enabled on close. The composite lives in
// fb0's memory, which nothing else is drawing into meanwhile -- SDL runs on
// its dummy video driver in this mode (main.cpp sets it before SDL_Init),
// so no EGL window is ever created on fb0.
//
// Layouts: the layer's source is a canvas in DS pixels laid out by
// Display::layout() at the primary screen's native size -- stacked, side by
// side, single, PiP, dominant: the same rectangles the SDL path uses, with
// the canvas standing in for the window. A 1:1 view is a NEON rotate; a
// smaller one (the PiP inset, the dominant layouts' secondary) is a box
// downscale into a cached temporary and row copies. The scaler then fits the
// whole canvas to the panel, which is what the layouts' own fit would have
// done. One layer, because the A33 has one scaler: the driver accepts a
// second layer in scaler mode but shows its source unscaled (seen on the
// unit), so per-view layers are not an option.
//
// What it does not do: the DE scaler filters, so the LCD grid, chunky and
// seam modes (CPU scanline features) do not apply; and there is no touch
// (the A30 has none).
#pragma once

#include "core/types.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace ds::sdl {

class DispOut {
public:
  static constexpr int BUFS = 3, VIEWS = 2;

  // True when /dev/disp and /dev/fb0 open and a layer query answers: the
  // device runs a disp-1.5 kernel. Cheap, cached; safe to call before SDL_Init.
  static bool available();

  // rot: 0, 90, 180 or 270, the rotation that takes the DS layout onto the
  // panel (DS_ROTATE on the spruce launcher: 270 on the A30). False leaves
  // nothing changed on the device.
  bool open(int rot, bool vsync);
  void close();
  bool vsync() const { return vsync_; }

  // The canvas: the layout's natural size at scale 1 (Display::natural_size),
  // in DS pixels before rotation. What layout() and map_point() work in;
  // logical_w/h() report it. At most two screens' worth of pixels.
  void set_canvas(int w, int h);
  int  logical_w() const { return canvas_w_; }
  int  logical_h() const { return canvas_h_; }

  // View i's rectangle on the canvas (Display::layout()'s views_, in order:
  // later views are drawn over earlier ones, so the PiP inset is last).
  void set_view(int i, int x, int y, int w, int h, bool shown);

  // Draws each view's 256x192 framebuffer (null skips the view) into a free
  // composite and flips the layer to it. Without vsync the flip is
  // immediate. With vsync the flip and the refresh wait happen on the
  // presenter thread: this returns as soon as the composite is drawn, never
  // blocking the emulation on the panel. A frame posted before the previous
  // one reached the panel replaces it (the panel shows the newest; nothing
  // waits), and the buffer being scanned out and the one latched for the
  // next refresh are never written -- three buffers cover displayed +
  // latched + the one being drawn.
  void present(const u32* const fb[VIEWS]);

private:
  struct ViewRect { int x = 0, y = 0, w = 0, h = 0; bool shown = false; };
  struct Dims { int w = 0, h = 0; };          // a composite's size (the canvas, rotated)
  bool set_layer(u32 addr, Dims d);
  void flip(int buf);
  void wait_vsync();
  void draw_view(u32* comp, int comp_w, const ViewRect& r, const u32* fb);
  void presenter();
  u32  buf_addr(int buf) const { return phys_ + static_cast<u32>(buf * buf_bytes_); }
  u32* buf_ptr(int buf) const { return reinterpret_cast<u32*>(map_ + buf * buf_bytes_); }

  int  disp_ = -1, fb_ = -1;
  u8*  map_ = nullptr;
  size_t map_len_ = 0;
  u32  phys_ = 0;
  int  panel_w_ = 0, panel_h_ = 0;
  int  rot_ = 0;
  int  canvas_w_ = 0, canvas_h_ = 0;
  size_t buf_bytes_ = 0;            // one composite's allocation (the largest canvas)
  int  layer_ = -1, ui_layer_ = -1;
  bool ui_was_enabled_ = false;
  bool layer_enabled_ = false;      // our layer is on (enabled on the first flip)
  ViewRect views_[VIEWS];
  Dims dims_[BUFS];                 // what each buffer holds, set by present, read by flip
  unsigned dirty_ = 0;              // buffers to black out before the next draw (canvas changed)
  std::vector<u32> tmp_, tmp2_;     // cached temporaries for the downscaled views
  int  cur_ = 0;
  bool vsync_ = true;
  bool pan_blocks_ = true;          // FBIOPAN_DISPLAY waits for the refresh (measured once)
  bool pan_measured_ = false;
  u64  next_ns_ = 0;                // fallback pacing when pan does not block

  // Presenter thread state (vsync only). Buffer indices; -1 = none.
  std::thread             thread_;
  std::mutex              mu_;
  std::condition_variable cv_;
  int  displayed_ = 0;              // on the panel now
  int  latched_ = -1;               // flipped to, waiting for the refresh
  int  pending_ = -1;               // drawn into, not yet flipped
  bool stop_ = false;
};

} // namespace ds::sdl
