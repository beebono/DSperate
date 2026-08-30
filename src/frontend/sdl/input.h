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
class Config;

// Things a hotkey can do. The frontend drains them once a frame.
enum class Action : u8 {
  Quit, Pause, FastForward, FastForwardToggle, SaveState, LoadState, SlotNext, SlotPrev,
  VolumeUp, VolumeDown, Mute, LayoutNext, LayoutPrev, ScreenSwap, PipCornerNext, Fullscreen, Screenshot, Lid, Mic, Count
};
const char* action_name(Action a);

// Keyboard, game controller and touch (real finger or mouse) folded into the
// DS's button mask and pen position, which are handed to the core once a frame.
// Bindings come from the config ([keys], [pad], [hotkeys], [padhotkeys]).
class Input {
public:
  void configure(const Config& cfg);
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
    const input::Frame f{static_cast<u16>(buttons_ | pressed_ | stick_), static_cast<u8>(touch_x_), static_cast<u8>(touch_y_), touching_ || touched_ || stylus_down_};
    pressed_ = 0; touched_ = false;
    return f;
  }

  // Hotkey actions since the last call, in order.
  std::vector<Action> take_actions() { std::vector<Action> a; a.swap(actions_); return a; }
  bool fast_forward_held() const { return ff_key_ || ff_pad_; }

  // Hinge: a real lid switch (lid.h) drives set_lid() directly; the `lid`
  // hotkey toggles it. Closing sends the game to sleep, opening wakes it.
  void set_lid(bool closed) { lid_ = closed; }
  bool lid() const { return lid_; }
  // Fake microphone for devices without one: `mic` held = noise at ~80 % of
  // full scale, otherwise silence. Fills `out` for one frame when active.
  bool fake_mic() const { return mic_key_ || mic_pad_; }
  void fake_mic_frame(std::vector<s16>& out);

  // Stick-driven pen: moved once a frame by the right stick's deflection
  // (pad.stylus_speed pixels per frame at full tilt); the frontend draws a
  // crosshair there while a controller is open.
  void update_stylus();
  bool stylus_visible() const { return stylus_stick_ && pad_ != nullptr && stylus_idle_ < stylus_hide_; }   // hidden after stylus_hide idle frames
  int  stylus_x() const { return static_cast<int>(stylus_fx_); }
  int  stylus_y() const { return static_cast<int>(stylus_fy_); }
  int  stylus_size() const { return stylus_size_; }

  bool quit() const { return quit_; }
  void request_quit() { quit_ = true; }

private:
  // One binding: a keyboard key, a pad button or a pad axis direction, with
  // or without the modifier.
  // `with` is a second pad button that must be held (a chord such as
  // mod+start+select); the most specific matching binding wins.
  struct Bind { enum Kind : u8 { None, Key, PadButton, PadAxis } kind = None; int code = 0; bool neg = false; bool mod = false; int with = -1; };
  static Bind parse_key(const std::string& s);
  static Bind parse_pad(const std::string& s);

  void set(io::Io::Button b, bool down) {
    if (down) { buttons_ |= 1u << b; pressed_ |= 1u << b; } else buttons_ &= ~(1u << b);
  }
  void touch_at(int wx, int wy, Display& display);
  void fire(Action a, bool down);
  bool key_down(SDL_Keycode k, bool down);
  bool pad_down(const Bind& b, bool down);   // a button or axis edge; true if consumed
  void axis(Uint8 which, Sint16 value);

  u32  buttons_ = 0, pressed_ = 0, stick_ = 0;   // held now; pressed since the last frame; stick as d-pad
  bool touching_ = false, touched_ = false;
  int  touch_x_ = 0, touch_y_ = 0;
  bool quit_ = false;
  bool lid_ = false, mic_key_ = false, mic_pad_ = false, ff_key_ = false, ff_pad_ = false;
  u32  noise_ = 0x2545F491;
  SDL_GameController* pad_ = nullptr;

  Bind key_map_[io::Io::BTN_COUNT];
  Bind pad_map_[io::Io::BTN_COUNT];
  Bind key_hot_[static_cast<int>(Action::Count)];
  Bind pad_hot_[static_cast<int>(Action::Count)];
  Bind key_mod_, pad_mod_;
  bool key_mod_down_ = false, pad_mod_down_ = false, pad_mod_used_ = false;
  int  pad_mod_button_ = -1;   // the DS button the pad modifier would otherwise be
  bool axis_state_[SDL_CONTROLLER_AXIS_MAX][2] = {};   // per axis: - and + past the threshold
  bool stick_dpad_ = true, stylus_stick_ = true, stylus_down_ = false;
  Bind stylus_button_;         // pressing it touches at the stick's position
  u32  held_ = 0;              // SDL pad buttons currently down (bit per button)
  int  deadzone_ = 12000;
  int  stylus_x_ = 0, stylus_y_ = 0;          // raw stick
  double stylus_fx_ = 128, stylus_fy_ = 96;    // pen position, DS pixels
  double stylus_speed_ = 4.0;
  int  stylus_size_ = 2;
  int  stylus_hide_ = 90, stylus_idle_ = 1 << 30;   // frames without movement or a touch; starts hidden
  std::vector<Action> actions_;
};

} // namespace ds::sdl
