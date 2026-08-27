// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "display_wl.h"   // complete type for the unique_ptr

#include <SDL2/SDL.h>

#include <memory>
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

  // Screens stacked (top over bottom) or side by side (top left, bottom
  // right): the latter matches handhelds whose two panels sit horizontally
  // in the compositor's canvas, so touch coordinates line up.
  enum class Layout { Vertical, Horizontal };
  bool open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, Layout layout = Layout::Vertical, bool accel = false);
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
  // Renderer-based drawing (draw(), --accel) is unavailable while this is on.
  //
  // Destination tiers, tried in order at open():
  //  1. dmabuf (display_wl.h): CMA buffers on SDL's Wayland surface. The
  //     compositor samples them zero-copy, or scans them out directly when
  //     the surface qualifies. DS_DMABUF=0 disables, =1 requires (fail loud).
  //  2. window surface: SDL's shm path. Always available under a compositor.
  struct Target { u32* px; u32 pitch; u32 h; const u16* xrun; };

  bool scaling() const { return scaled_; }
  // Locks the panel-sized texture and fills in one target per screen. False
  // if the lock failed, in which case the caller must fall back to draw().
  bool begin_frame(Target out[SCREENS]);
  void end_frame();   // unlock and present
  void on_resize() { layout(); build_scale(); margins_dirty_ = true; }
  void toggle_fullscreen();

  // Window point -> pixel in `screen`. False if the point is not on a screen.
  bool map_point(int wx, int wy, int& screen, int& sx, int& sy) const;

  // Renderer output size, which is what map_point's coordinates are in (it
  // differs from the window size on scaled displays).
  void output_size(int& w, int& h) const { out_size(w, h); }
  SDL_Window* window() const { return win_; }
  u32 window_id() const { return win_ ? SDL_GetWindowID(win_) : 0; }

private:
  struct View { int screen; SDL_Rect rect; };

  void layout();
  void build_scale();          // pick up the window surface and rebuild the x-map
  bool out_size(int& w, int& h) const;   // renderer output, or the surface in scaled mode
  void clear_margins(u32* px, u32 pitch) const;

  SDL_Window*   win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture*  tex_[SCREENS] = {nullptr, nullptr};
  View          views_[SCREENS] = {};
  bool          fullscreen_ = false;
  Layout        layout_ = Layout::Vertical;

  bool              scaled_ = false;
  std::unique_ptr<DmabufOut> dm_;  // tier 1; null on the surface tier
  SDL_Surface*      surf_ = nullptr;    // window surface; owned by SDL
  bool              margins_dirty_ = true;
  int               dm_margins_ = 0;      // dmabuf buffers whose letterbox is cleared
  bool              dm_frame_ = false;    // current begin_frame targeted the dmabuf
  int               scaled_w_ = 0, scaled_h_ = 0;
  std::vector<u16>  xrun_;     // 257 entries; see kern::scale_row
};

} // namespace ds::sdl
