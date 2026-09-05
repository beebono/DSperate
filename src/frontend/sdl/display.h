// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/gpu.h"
#include "core/types.h"
#include "display_disp.h"
#include "display_drm.h"  // complete types for the unique_ptr
#include "display_fbdev.h"
#include "display_wl.h"

#include <SDL2/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace ds::sdl {

// One window showing some of the DS screens.
//
// The screens are drawn as a list of views (which screen goes in which
// rectangle), so a second Display showing one screen each is a matter of
// creating two of these — textures belong to a renderer and cannot be shared
// between windows, which is why the texture lives here and not in the app.
// The handhelds run SDL's KMSDRM backend, where only one window exists, so
// one window holds both screens, stacked or side by side.
class Display {
public:
  static constexpr int SCREENS = 2;

  // How the two screens share the window. Every mode keeps the 4:3 screen
  // aspect; `primary` is the screen shown alone (Single), large (Pip) or
  // dominant (DominantV/H), and the first one in the stack/row otherwise.
  //   Vertical    stacked, top over bottom (top=primary first)
  //   Horizontal  side by side: matches handhelds whose two panels sit
  //               horizontally in the compositor's canvas, so touch lines up
  //   Single      one screen fills the window; the other is not drawn
  //   Pip         one fills the window, the other is an inset in a corner
  //   DominantV   stacked in DS order, the primary fitted to the width and
  //               the other `dominant` times its size, both centred
  //   DominantH   side by side in DS order, the primary fitted to the
  //               height, the other `dominant` times its size, bottoms aligned
  enum class Mode : u8 { Vertical, Horizontal, Single, Pip, DominantV, DominantH, Count };
  enum class Corner : u8 { TopLeft, TopRight, BottomLeft, BottomRight, Count };
  struct Layout {
    Mode   mode = Mode::Vertical;
    int    primary = 0;
    Corner corner = Corner::BottomRight;
    double pip = 1.0 / 3.0;      // inset size relative to the large screen
    double dominant = 0.5;       // secondary size relative to the dominant screen
    double pip_alpha = 1.0;      // inset opacity at rest, 0..1 (see set_inset_alpha)
  };
  static const char* mode_name(Mode m);      // "vertical" ... "dominant_h"
  static bool parse_mode(const std::string& s, Mode& m);
  static const char* corner_name(Corner c);  // "tl" "tr" "bl" "br"
  static bool parse_corner(const std::string& s, Corner& c);
  // The window size that shows the layout at `scale` window pixels per DS pixel.
  static void natural_size(const Layout& l, double scale, int& w, int& h);

  bool open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, const Layout& layout, int only_screen = -1, int display_index = 0);
  void close();

  void draw(const u32* const fb[SCREENS]);

  // Per-scanline scaling. Instead of the core filling a 256x192 framebuffer
  // that we then rescale to the panel, we hand the core a panel-sized buffer
  // and it scales each line into place as that line is produced, while the
  // line is still in L1. The rescale pass then disappears entirely, along
  // with the cold re-read of the framebuffer that dominated it, and the
  // present becomes a 1:1 copy.
  //
  // The destination has to be the buffer that is actually presented, or the
  // saving is spent again on a copy: scaling into a panel-sized texture still
  // leaves SDL_RenderCopy reading 3.6 MB and writing 3.6 MB, where the
  // framebuffer path's RenderCopy reads only 192 KB. So this mode drops
  // SDL_Renderer and renders into the window surface, which on Wayland is the
  // shm buffer SDL commits. That is also the shape the compositor-bypass path
  // needs, with a dma-heap allocation in place of the shm buffer; doing it
  // here first measures the scaling win on its own, on the path that ships.
  //
  // Renderer-based drawing (draw()) is unavailable while this is on.
  //
  // Destination tiers, tried in order at open():
  //  1. a scanout tier (scanout.h): CMA dma-heap buffers the display samples
  //     or scans out without a copy. Under Wayland that is DmabufOut, on
  //     SDL's Wayland surface; under KMSDRM it is DrmOut, page-flipped onto
  //     the panel's CRTC. DS_DMABUF=0 disables either, =1 requires it
  //     (fail loud).
  //  2. window surface: SDL's shm path under a compositor. On KMSDRM this is
  //     not a software path at all -- SDL has no window framebuffer there, so
  //     it is a hidden GLES renderer; see display_drm.h.
  struct Target { u32* px; u32 pitch; u32 h; const u16* xrun; const u8* seam_w; };

  bool scaling() const { return scaled_; }
  // Chunky: each 2x2 block of DS pixels is drawn as one cell from its top-left
  // pixel (with the LCD grid, one seam per block). Set before open().
  // cell: 0 = the 2x2 pair path; N = N panel pixels per cell when N divides
  // both screen dimensions (else the pair path); -1 = auto: the smallest
  // N >= 4 that does, up to 16.
  void set_chunky(bool on, int cell = 0) { chunky_ = on; chunky_cell_ = cell; }
  bool chunky() const { return chunky_; }
  // Present through the display engine's scaler layer (display_disp.h) when
  // the device has one; the DS_ROTATE environment gives the panel rotation.
  // Set before open(); draw() is then the way to the screen.
  void set_disp(bool on) { disp_wanted_ = on; }
  bool disp() const { return disp_ != nullptr; }
  // Present straight through /dev/fb0 (display_fbdev.h): the scanline path
  // writes panel-sized frames into fb0's own buffers. Set before open().
  void set_fbdev(bool on) { fbdev_wanted_ = on; }
  // The cell map in use for `screen` (null when the pair path is), for the core.
  const void* cell_map(int screen) const { return cells_[screen].x.cells ? &cells_[screen] : nullptr; }
  // Locks the panel-sized texture and fills in one target per screen. False
  // if the lock failed, in which case the caller must fall back to draw().
  bool begin_frame(Target out[SCREENS]);
  // `screen` as last presented, sampled back to 256x192 into `dst`, from the
  // buffer the scanline path last drew into. On a scanline tier the core's
  // own framebuffers are never written (the lines go straight to the panel),
  // so a screenshot has to come from here. False on the renderer tier, or
  // before the first frame: read the core's framebuffer instead.
  bool read_screen(int screen, u32* dst) const;
  void end_frame();   // unlock and present
  void on_resize() { layout(); build_scale(); margins_dirty_ = true; }
  void toggle_fullscreen();
  // Switches layout; a windowed window is resized to the new mode's natural
  // size at the current scale. Ignored on a single-screen (dual-window)
  // display.
  void set_layout(const Layout& l);
  const Layout& current_layout() const { return layout_; }
  // The inset's opacity for the coming frame, 0..255: the frontend ramps it
  // between Layout::pip_alpha and opaque while the bottom screen is touched.
  // Below 255 the inset is blended over the large screen on every tier;
  // at 255 it is copied, and the exact paths cost nothing extra.
  void set_inset_alpha(u8 a) { inset_alpha_ = a; }
  u8   inset_alpha() const { return inset_alpha_; }
  // dst[i] = dst[i] + (src[i] - dst[i]) * alpha / 255 per channel, alpha
  // forced opaque. Shared with the screenshot writer.
  static void blend_row(u32* dst, const u32* src, size_t n, u32 alpha);
  bool across() const { return layout_.mode == Mode::Horizontal || layout_.mode == Mode::DominantH; }

  // Window point -> pixel in `screen`. False if the point is not on a screen.
  bool map_point(int wx, int wy, int& screen, int& sx, int& sy) const;

  // Renderer output size, which is what map_point's coordinates are in (it
  // differs from the window size on scaled displays).
  void output_size(int& w, int& h) const { out_size(w, h); }
  SDL_Window* window() const { return win_; }
  u32 window_id() const { return win_ ? SDL_GetWindowID(win_) : 0; }

  // `direct`: the core scales straight into the window at `rect`. Otherwise
  // it scales into side_[screen] (an inset that would be overwritten by the
  // screen under it, or a hidden screen -- the core needs a target for
  // both), which end_frame() copies into place if `shown`.
  struct View { int screen; SDL_Rect rect; bool direct; bool shown; };
  // Where the two screens go in a w x h output under `l`, in draw order
  // (later views on top). Shared with the screenshot writer, so a picture
  // of the layout is laid out exactly as the window is.
  static void place(const Layout& l, int w, int h, View out[SCREENS]);

private:
  void layout();
  void build_scale();          // pick up the window surface and rebuild the x-map
  bool out_size(int& w, int& h) const;   // renderer output, or the surface in scaled mode
  void clear_margins(u32* px, u32 pitch, int w, int h) const;
  void targets(u32* px, u32 stride, Target out[SCREENS]);
  void blit_insets();

  SDL_Window*   win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture*  tex_[SCREENS] = {nullptr, nullptr};
  View          views_[SCREENS] = {};
  int           nviews_ = SCREENS;
  int           only_screen_ = -1;
  int           display_index_ = 0;
  bool          fullscreen_ = false;
  Layout        layout_;
  u8            inset_alpha_ = 255;

  bool              scaled_ = false;
  bool              disp_wanted_ = false;
  bool              fbdev_wanted_ = false;
  std::unique_ptr<DispOut> disp_;       // display-engine tier; null otherwise
  bool              chunky_ = false;
  int               chunky_cell_ = 0;
  std::unique_ptr<ScanoutOut> out_;     // tier 1; null on the surface tier
  SDL_Surface*      surf_ = nullptr;    // window surface; owned by SDL
  bool              margins_dirty_ = true;
  int               out_margins_ = 0;     // scanout buffers whose letterbox is cleared
  bool              out_frame_ = false;   // current begin_frame targeted the scanout tier
  int               scaled_w_ = 0, scaled_h_ = 0;
  std::vector<u16>  xrun_[SCREENS];   // per screen, 257 entries; see kern::scale_row
  std::vector<u8>   seam_w_[SCREENS]; // per screen, 256 entries: box-filter weight of pixel s+1 in run s's last pixel
  ds::gpu::Gpu::CellMap cells_[SCREENS];   // chunky cell tables; x.cells == 0 when the pair path is in use
  std::vector<u32>  side_[SCREENS];   // scaled pixels of a non-direct view
  u32*              frame_px_ = nullptr;   // the buffer begin_frame handed out, for end_frame's insets
  u32               frame_pitch_ = 0;
  const u32*        last_px_ = nullptr;    // the last such buffer, for read_screen (still holds that frame)
  u32               last_pitch_ = 0;
};

} // namespace ds::sdl
