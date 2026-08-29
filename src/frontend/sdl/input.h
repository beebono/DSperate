// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/io/io.h"
#include "core/input/input_log.h"

#include <SDL2/SDL.h>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

class Display;

// Keyboard, game controller and touch (real finger or mouse) folded into the
// DS's button mask and pen position, which are handed to the core once a frame.
class Input {
public:
  void open_controllers();
  void close();

  // Feeds one SDL event; `display` maps window points onto the screens. In
  // dual-window mode `second` is the other window: pointer, touch and window
  // events are routed to whichever owns the event's windowID.
  void handle(const SDL_Event& e, Display& display, Display* second = nullptr);
  // The state for the coming frame. A press and release that both arrived
  // since the last frame (a quick tap between two polls, common when frames
  // take 30 ms) still count as held for this frame: the release lands on
  // the next one, so the game sees every tap.
  input::Frame frame() {
    const input::Frame f{static_cast<u16>(buttons_ | pressed_), static_cast<u8>(touch_x_), static_cast<u8>(touch_y_), touching_ || touched_};
    pressed_ = 0; touched_ = false;
    return f;
  }

  // Hinge: `L` toggles it from the keyboard; a real lid switch (lid.h) drives
  // set_lid() directly. Closing sends the game to sleep, opening wakes it.
  void set_lid(bool closed) { lid_ = closed; }
  bool lid() const { return lid_; }
  // Fake microphone for devices without one: `M` held = noise at ~80 % of
  // full scale, otherwise silence. Fills `out` for one frame when active.
  bool fake_mic() const { return mic_key_ || mic_trigger_; }
  void fake_mic_frame(std::vector<s16>& out);

  bool quit() const { return quit_; }
  // Select+Start together quits when there is no keyboard (handhelds).
  bool combo_quit() const { return (buttons_ & (1u << io::Io::BTN_SELECT)) && (buttons_ & (1u << io::Io::BTN_START)); }

private:
  void set(io::Io::Button b, bool down) {
    if (down) { buttons_ |= 1u << b; pressed_ |= 1u << b; } else buttons_ &= ~(1u << b);
  }
  void touch_at(int wx, int wy, Display& display);

  u32  buttons_ = 0, pressed_ = 0;        // held now; pressed since the last frame
  bool touching_ = false, touched_ = false;
  int  touch_x_ = 0, touch_y_ = 0;
  bool quit_ = false;
  bool lid_ = false, mic_key_ = false, mic_trigger_ = false;
  u32  noise_ = 0x2545F491;
  SDL_GameController* pad_ = nullptr;
};

} // namespace ds::sdl
