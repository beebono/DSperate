// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/io/io.h"
#include "core/input/input_log.h"

#include <SDL2/SDL.h>

namespace ds { struct NDS; }
namespace ds::sdl {

class Display;

// Keyboard, game controller and touch (real finger or mouse) folded into the
// DS's button mask and pen position, which are handed to the core once a frame.
class Input {
public:
  void open_controllers();
  void close();

  // Feeds one SDL event; `display` maps window points onto the screens.
  void handle(const SDL_Event& e, Display& display);
  input::Frame frame() const { return input::Frame{static_cast<u16>(buttons_), static_cast<u8>(touch_x_), static_cast<u8>(touch_y_), touching_}; }

  bool quit() const { return quit_; }
  // Select+Start together quits when there is no keyboard (handhelds).
  bool combo_quit() const { return (buttons_ & (1u << io::Io::BTN_SELECT)) && (buttons_ & (1u << io::Io::BTN_START)); }

private:
  void set(io::Io::Button b, bool down) {
    if (down) buttons_ |= 1u << b; else buttons_ &= ~(1u << b);
  }
  void touch_at(int wx, int wy, Display& display);

  u32  buttons_ = 0;
  bool touching_ = false;
  int  touch_x_ = 0, touch_y_ = 0;
  bool quit_ = false;
  SDL_GameController* pad_ = nullptr;
};

} // namespace ds::sdl
