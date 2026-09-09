// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Presentation through the Allwinner display engine's scaler layer, for the
// Miyoo A30 (Allwinner A33, "disp 1.5" driver) and anything else that
// answers the same /dev/disp ioctls.
//
// Why this tier exists: the A30's SDL2 has one video driver, Mali EGL over
// fbdev, and every SDL path to the panel is a GLES upload and swap (3 ms per
// frame through the GLES renderer, 97 ms through the window surface). The panel is also
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
// Effects: chunky is the scaler's own doing wherever the cells come out
// whole (set_divisor): the composite is the canvas at cell resolution --
// each 1:1 view box-downscaled by the cell size, the same path the insets
// take -- and the DE enlarges it with the nearest table, so a 2x2 DS block
// is exactly one 5x5 panel cell in the single layout with no per-pixel
// work at DS resolution. Where a view does not divide (the PiP inset)
// Display falls back to flattening at DS resolution before the rotate (it
// runs the scanline scaler at 1:1 into a side buffer). The LCD grid is a
// second DE layer, a static panel-sized ARGB image blended per pixel over
// the scaled composite (set_grid). The seam blend modes need panel pixels
// and do not apply.
//
// Filter: the scaler is a polyphase FIR (32 phases, 8-bit weights summing
// to 64) whose coefficient RAM the disp driver fills with its own smoothing
// table on every layer set -- the ioctls offer no say in it. Nearest
// neighbour (the default, as on every other tier) is a table with the whole
// weight on one tap, written into that RAM through /dev/mem right after
// each layer set: the driver's write happens inside the ioctl and the
// scaler reads the RAM live, so ours is what shows from the next line on
// (verified on the A30; the driver's vblank handler only sets reg_rdy).
// With --linear the RAM is left to the driver and its filter is what shows.
// No touch (the A30 has none).
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
  static constexpr int BUFS = 4, VIEWS = 2;

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
  // The chunky cell drawn at source in view i, in canvas pixels (1 = none):
  // the grid puts one seam per cell there.
  void set_view_cell(int i, int cell);
  // Hardware chunky: the composite is the canvas divided by d (1 = off);
  // every view's rectangle and the canvas must divide by d. A 1:1 view is
  // then box-downscaled by d (its cells' mean) and the DE enlarges the
  // result -- with nearest, integer panel cells. Insets are drawn opaque
  // under a divisor (the translucent path reads the 1:1 view's pixels).
  void set_divisor(int d);
  int  divisor() const { return div_; }
  // Panel pixels per canvas (DS) pixel under the current canvas: what the
  // DE's fit gives (Display sizes its chunky cells by it). Independent of
  // the divisor.
  double fit_scale() const;
  // Opacity of the views drawn over another (the PiP inset), 0..255: below
  // 255 a downscaled view is blended over what is already in the composite.
  // A 1:1 view is always drawn opaque (nothing lies under one).
  void set_inset_alpha(u8 a) { inset_alpha_ = a; }
  // The LCD grid as a second layer: a panel-sized ARGB image, transparent
  // but for one black pixel at `alpha` leading each DS pixel's run of panel
  // pixels (a chunky cell's, per set_view_cell), the
  // seam rule of kern::scale_row_grid. The DE blends it per pixel over the
  // scaled composite (pipe 0 over pipe 1, alpha_mode 0 -- probed on the A30),
  // so it costs nothing per frame; it is redrawn when the layout changes.
  // Set before open(); 0 = no grid.
  void set_grid(u8 alpha) { grid_alpha_ = alpha; }
  bool grid() const { return grid_layer_ >= 0 && grid_alpha_ != 0; }

  // The same layer as a drawing surface for the frontend (the pause menu, the
  // OSD): a panel-resolution ARGB image the DE blends per pixel over the
  // scaled picture, so the menu comes out at the panel's own resolution
  // instead of being upscaled with the frame -- what every other tier does
  // through Display::canvas().
  //
  // It shares the grid's layer and image because this chip offers no second
  // one, which the A30 was probed for: the DE has two pipes and per-pixel
  // alpha blends only ACROSS pipes, and both are taken (the scaler on 1, this
  // on 0); layers 1 and 2 are free but have nowhere to point, since fb0 has
  // 864 KB left after four composites and the overlay against the 1.2 MB a
  // second panel-sized image needs, and fb1-fb7 advertise 4.9 MB each but are
  // unallocated (smem_start 0, mmap fails). So the grid seams and whatever
  // the frontend draws are composed into one image.
  //
  // Costs nothing per frame: the image is recomposed only when the frontend
  // says it changed, and uploaded to fb0 only when the composition differs
  // from what is already there. Enable before open().
  void set_overlay(bool on) { overlay_wanted_ = on; }
  bool overlay_available() const { return grid_layer_ >= 0 && overlay_wanted_; }
  // The surface, in DISPLAY orientation (the panel turned by rot, so drawing
  // code works the way up the player holds it). Cleared to transparent on the
  // first call of a frame. False when there is no overlay.
  bool overlay(u32*& px, int& pitch, int& w, int& h);
  // Said after drawing: `any` false means nothing was drawn this frame, which
  // takes the last overlay back off the panel.
  void overlay_changed(bool any);
  // Nearest-neighbour scaling (see the header comment); set before open().
  // false leaves the driver's own filter in place (--linear).
  void set_nearest(bool on) { nearest_wanted_ = on; }
  // Display::IntScale as an int (0 off, 1 under, 2 over): the DE fit is
  // snapped to whole panel pixels per DS pixel; over crops the source window.
  void set_integer_scale(int m) { int_scale_ = m; }
  bool nearest() const { return fe_ != nullptr; }

  // Draws each view's 256x192 framebuffer (null skips the view) into a free
  // composite and flips the layer to it. Without vsync the flip is
  // immediate. With vsync the flip and the refresh wait happen on the
  // presenter thread: this returns as soon as the composite is drawn, never
  // blocking the emulation on the panel. A frame posted before the previous
  // one reached the panel replaces it (the panel shows the newest; nothing
  // waits), and the buffer being scanned out, the one latched for the next
  // refresh and the one waiting for the blank are never written -- four
  // buffers cover displayed + latched + queued + the one being drawn.
  void present(const u32* const fb[VIEWS]);

private:
  struct ViewRect { int x = 0, y = 0, w = 0, h = 0; bool shown = false; int cell = 1; };
  struct Dims { int w = 0, h = 0; };          // a composite's size (the canvas, rotated, over the divisor)
  Dims comp_dims() const;
  bool set_layer(u32 addr, Dims d);
  // The panel window the composite is fitted into (aspect kept, centred),
  // and the part of the composite shown in it: all of it, unless integer
  // overscale crops it (then a centred window of whole composite pixels).
  struct Fit { int x = 0, y = 0; unsigned w = 0, h = 0; int sx = 0, sy = 0; unsigned sw = 0, sh = 0; };
  Fit fit(Dims d) const;
  double snap(double s) const;
  // View r's rectangle in the composite (rotated), as draw_view places it.
  void comp_rect(const ViewRect& r, int& cx, int& cy, int& cw, int& ch) const;
  void draw_grid(Dims d);
  bool set_grid_layer();
  void flip(int buf);
  void wait_vsync();
  bool open_frontend();           // maps the DE front end's registers for write_coefs()
  void write_coefs();             // the nearest table into the scaler's coefficient RAM
  void draw_view(u32* comp, int comp_w, const ViewRect& r, const u32* fb, int index, const u32* const fbs[VIEWS]);
  // The canvas point a composite point came from (the inverse of draw_view's
  // rotation), and the 1:1 view under it drawn before `index`, if any.
  void canvas_point(int compx, int compy, int& x, int& y) const;
  const u32* under_pixel(int x, int y, int index, const u32* const fbs[VIEWS]) const;
  void presenter();
  u32  buf_addr(int buf) const { return phys_ + static_cast<u32>(buf * buf_bytes_); }
  u32* buf_ptr(int buf) const { return reinterpret_cast<u32*>(map_ + buf * buf_bytes_); }

  int  disp_ = -1, fb_ = -1, mem_ = -1;
  volatile u32* fe_ = nullptr;      // DE front end registers (nearest only)
  bool nearest_wanted_ = true;
  int  int_scale_ = 0;
  u8*  map_ = nullptr;
  size_t map_len_ = 0;
  u32  phys_ = 0;
  int  panel_w_ = 0, panel_h_ = 0;
  int  rot_ = 0;
  int  canvas_w_ = 0, canvas_h_ = 0;
  int  div_ = 1;                    // set_divisor
  size_t buf_bytes_ = 0;            // one composite's allocation (the largest canvas)
  int  layer_ = -1, ui_layer_ = -1;
  u8   grid_alpha_ = 0;
  int  grid_layer_ = -1;            // the grid's layer; -1 = no grid
  bool grid_enabled_ = false;
  bool grid_dirty_ = false;         // redraw the grid image before the next flip
  size_t grid_off_ = 0;             // the grid image's offset in fb0 memory (after the composites)
  Dims grid_dims_;                  // the composite size the image was drawn for
  std::vector<u32> grid_stage_;     // the image is composed here, then copied to fb0 in bulk
  bool overlay_wanted_ = false;
  bool ov_active_ = false;          // the frontend drew something last frame
  bool ov_taken_ = false;           // overlay() was called since the last compose
  int  ov_w_ = 0, ov_h_ = 0;        // the surface's size, in display orientation
  std::vector<u32> ov_stage_;       // what the frontend draws into (display orientation)
  std::vector<u32> ov_sent_;        // the last image uploaded to fb0, to skip identical ones
  void compose_overlay();           // grid seams + ov_stage_, rotated, into fb0 if it changed
  bool ui_was_enabled_ = false;
  bool layer_enabled_ = false;      // our layer is on (enabled on the first flip)
  ViewRect views_[VIEWS];
  u8   inset_alpha_ = 255;
  Dims dims_[BUFS];                 // what each buffer holds, set by present, read by flip
  unsigned dirty_ = 0;              // buffers to black out before the next draw (canvas changed)
  std::vector<u32> tmp_, tmp2_, tmp3_, tmp4_;   // cached temporaries for the downscaled views (tmp3_: the row under a blended one; tmp4_: a halving pass)
  int  cur_ = 0;
  bool vsync_ = true;
  bool timing_ = false;             // DS_DISP_TIMING: log the flip's distance from the blank
  bool diag_ = false;               // DS_DISP_DIAG: stall and rate diagnostics from the presenter
  u64  diag_flips_ = 0, diag_posts_ = 0, diag_mark_ = 0;
  u64  flip_ns_sum_ = 0, flip_ns_max_ = 0, flip_n_ = 0, flip_miss_ = 0;
  bool pan_blocks_ = true;          // FBIOPAN_DISPLAY waits for the refresh (measured once)
  bool pan_measured_ = false;
  u64  next_ns_ = 0;                // fallback pacing when pan does not block

  // Presenter thread state (vsync only). Buffer indices; -1 = none.
  std::thread             thread_;
  std::mutex              mu_;
  std::condition_variable cv_;
  int  displayed_ = 0;              // on the panel now
  int  latched_ = -1;               // flipped to, waiting for the refresh
  int  queued_ = -1;                // taken by the presenter, waiting for the vsync to flip
  int  pending_ = -1;               // drawn into, not yet flipped
  bool stop_ = false;
};

} // namespace ds::sdl
