// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <SDL2/SDL.h>

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
  void on_resize() { layout(); }
  void toggle_fullscreen();

  // Window point -> pixel in `screen`. False if the point is not on a screen.
  bool map_point(int wx, int wy, int& screen, int& sx, int& sy) const;

  // Renderer output size, which is what map_point's coordinates are in (it
  // differs from the window size on scaled displays).
  void output_size(int& w, int& h) const { SDL_GetRendererOutputSize(ren_, &w, &h); }
  SDL_Window* window() const { return win_; }
  u32 window_id() const { return win_ ? SDL_GetWindowID(win_) : 0; }

private:
  struct View { int screen; SDL_Rect rect; };

  void layout();

  SDL_Window*   win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_Texture*  tex_[SCREENS] = {nullptr, nullptr};
  View          views_[SCREENS] = {};
  bool          fullscreen_ = false;
  Layout        layout_ = Layout::Vertical;
};

} // namespace ds::sdl
