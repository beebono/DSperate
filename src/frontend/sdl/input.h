// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/io/io.h"
#include "core/input/input_log.h"

#include <SDL2/SDL.h>
#include <string>
#include <vector>

namespace ds { struct NDS; }
namespace ds::sdl {

class Display;
class Config;

// Things a hotkey can do. The frontend drains them once a frame.
enum class Action : u8 {
  Quit, Pause, FastForward, FastForwardToggle, SaveState, LoadState, SlotNext, SlotPrev,
  VolumeUp, VolumeDown, Mute, LayoutNext, LayoutPrev, ScreenSwap, PipCornerNext, Fullscreen, Screenshot, Lid, Mic, FpsToggle, Count
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
    const input::Frame f{static_cast<u16>(buttons_ | pressed_ | stick_ | face_stick_), static_cast<u8>(touch_x_), static_cast<u8>(touch_y_), touching_ || touched_ || stylus_down_ != 0};
    pressed_ = 0; stick_pressed_ = 0; touched_ = false;
    // The menu's fallback presses (menu_fallback) never reach the guest, and
    // they die with the frame they happened on: an unbound Escape pressed
    // during play must not still be waiting to cancel the next menu opened.
    menu_fb_pressed_ = 0;
    return f;
  }

  // Hotkey actions since the last call, in order.
  std::vector<Action> take_actions() { std::vector<Action> a; a.swap(actions_); return a; }

  // Pause menu: the presses since the last call, as a DS button mask, for a
  // frontend that is driving a menu instead of the game. The player's own
  // bindings navigate it -- up/down/A/B are whatever they mapped -- and the
  // guest never sees them, because the menu is only up while paused and
  // frame() is not being called. Edge-triggered: a held direction moves one
  // row, the same as the taps frame() is built to catch.
  u32 take_menu_presses() {
    const u32 p = pressed_ | stick_pressed_ | menu_fb_pressed_;
    pressed_ = 0; stick_pressed_ = 0; menu_fb_pressed_ = 0;
    return p;
  }
  // What is held right now, for the menu's key repeat. Not the same as the
  // edges above: a direction held down produces one press and then nothing.
  u32 menu_held() const { return buttons_ | stick_ | menu_fb_held_; }
  bool fast_forward_held() const { return ff_key_ || ff_pad_; }

  // Hinge: a real lid switch (lid.h) drives set_lid() directly; the `lid`
  // hotkey toggles it. Closing sends the game to sleep, opening wakes it.
  void set_lid(bool closed) { lid_ = closed; }
  bool lid() const { return lid_; }
  // Fake microphone for devices without one: `mic` held = noise at ~80 % of
  // full scale, otherwise silence. Fills `out` for one frame when active.
  bool fake_mic() const { return mic_key_ || mic_pad_; }
  void fake_mic_frame(std::vector<s16>& out);

  // Pad-driven pen: moved once a frame by a stick's deflection (pad.stylus_axis,
  // pad.stylus_speed pixels per frame at full tilt) and by the d-pad while the
  // pad.stylus_dpad chord button is held; the frontend draws a crosshair
  // there while a controller is open.
  void update_stylus();
  bool stylus_visible_binding() const;   // some pad control drives the pen
  bool stylus_visible() const { return (stylus_axis_ != StylusAxis::None || stylus_chord_.kind != Bind::None) && pad_ != nullptr && stylus_idle_ < stylus_hide_; }   // hidden after stylus_hide idle frames
  int  stylus_x() const { return static_cast<int>(stylus_fx_); }
  int  stylus_y() const { return static_cast<int>(stylus_fy_); }
  int  stylus_size() const { return stylus_size_; }

  bool quit() const { return quit_; }
  void request_quit() { quit_ = true; }

  // Rebinding, for the Controls page.
  bool has_pad() const { return pad_ != nullptr; }
  // While capturing, the next physical key or pad control is taken as a name
  // instead of being fed to the game or matched against a hotkey -- otherwise
  // rebinding Quit would quit. `pad` picks which device is listened to, so
  // pressing a key does not land in the pad column.
  void begin_capture(bool pad);
  void cancel_capture() { capturing_ = false; }
  bool capturing() const { return capturing_; }
  // The name of what was pressed, once. Empty until then; taking it ends the
  // capture, so a caller polls this and stops when it gets something.
  std::string take_capture();
  // Every binding that would shadow another, as one line per clash, for the
  // page to show. Empty when there are none. The same rules warn_collisions()
  // prints at startup -- the player who is doing the rebinding is the one who
  // needs to be told.
  std::vector<std::string> collisions() const;
  // What the config calls each DS button and each hotkey action, and what it
  // falls back to when unset. The Controls page shows the config string
  // itself -- it is already the name of the control, and showing it means the
  // page and the file can never disagree about what a button is bound to.
  static const char* button_name(int i);
  static int button_count();
  static const char* key_default(int i);
  static const char* pad_default(int i);
  // A pad binding as the menu draws it (L1/SELECT/position pips), against the
  // SDL spelling the config file stores. Display only: nothing is written back
  // in this form.
  static std::string pad_label(const std::string& value);
  // The controls that are neither a DS button nor a hotkey, for the rows the
  // Controls page gives them: the modifier a pad chord is built on, and the
  // pen's tap button and stick.
  static const char* mod_default(bool pad);
  static const char* stylus_button_default();
  static const char* stylus_dpad_default();
  static const char* stick_face_default();
  static const char* stick_dpad_default();
  static const char* stylus_axis_default(const Config& cfg);
  // A captured stick axis as the stick it belongs to ("left"/"right"), or
  // nullptr when what was captured is not a stick at all.
  static const char* stylus_axis_of(const std::string& captured);
  static int action_count();
  static const char* key_hot_default(int i);
  static const char* pad_hot_default(int i);
  // A hotkey can carry a second binding in the same column, spelled
  // "<action>.alt" in the config (hotkeys.pause.alt, padhotkeys.pause.alt).
  // It has no built-in default -- the first binding is the layout the device
  // was designed around -- so it starts unset and only exists if the player
  // sets it. DS buttons have no second binding; the pen's tap button takes
  // one too (pad.stylus_button.alt), since a handheld often has two controls
  // that are comfortable to tap with.
  static constexpr int HOT_SLOTS = 2;
  static const char* hot_suffix(int slot) { return slot == 1 ? ".alt" : ""; }

  // The event-level paths below are what the menu's rescue behaviour lives in
  // (the fallback layer, the capture's escape), and they are reached from
  // handle(), which needs a Display and a window. tests/input_test.cpp drives
  // them directly instead.
  friend struct InputTestAccess;

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
  void warn_collisions() const;   // bindings that shadow one another, at configure time
  bool reachable(int ds_button) const;   // is there a control for it on the hardware in hand?
  bool bound_and_present(int ds_button) const;   // bound to a control this hardware has
  bool pad_control_free(int sdl_button) const;   // nothing bound to it, so the menu fallback has it
  bool key_control_free(SDL_Keycode k) const;
  void fire(Action a, bool down);
  bool key_down(SDL_Keycode k, bool down);
  bool pad_down(const Bind& b, bool down);   // a button or axis edge; true if consumed
  void axis(Uint8 which, Sint16 value);
  // A stick as four DS buttons (the left stick as the d-pad, or either as
  // X/B/Y/A): held while past the deadzone, with edges for the pause menu.
  void stick_as_buttons(u32& held, Sint16 value, io::Io::Button neg, io::Io::Button pos);
  bool capture_event(const SDL_Event& e);
  bool axis_moved(int axis, int value) const;    // past the deadzone this axis needs
  bool is_pad_mod_button(int sdl_button) const;   // the pad's hotkey modifier
  bool is_key_mod(SDL_Keycode k) const;
  // The menu is navigated by the player's own bindings, so a player who binds
  // DS A to nothing reachable can no longer reach the page that would put it
  // back. These are the controls the menu falls back on, and they are consulted
  // only when key_down()/pad_down() report that nothing at all is bound to the
  // control: a control the player has given a job keeps that job, and cannot
  // fire two menu actions at once. Under the shipped defaults every entry
  // below is bound, so the fallback contributes nothing until something is
  // orphaned.
  void menu_fallback_key(SDL_Keycode k, bool down);
  void menu_fallback_pad(const Bind& b, bool down);
  void menu_fallback(int ds_button, bool down) {
    if (down) { menu_fb_held_ |= 1u << ds_button; menu_fb_pressed_ |= 1u << ds_button; }
    else menu_fb_held_ &= ~(1u << ds_button);
  }

  u32  buttons_ = 0, pressed_ = 0, stick_ = 0;   // held now; pressed since the last frame; stick as d-pad
  u32  stick_prev_ = 0, stick_pressed_ = 0;      // stick-as-d-pad edges, for the pause menu
  // The face-button stick is the game's only: on the Controls page X resets
  // the column and B leaves it, and a stick that fired those while the
  // player was lining up a binding would undo it. The d-pad stick still
  // navigates the menu, as it always has.
  u32  face_stick_ = 0;
  u32  menu_fb_held_ = 0, menu_fb_pressed_ = 0;  // menu-only fallback, for orphaned bindings
  bool touching_ = false, touched_ = false;
  int  touch_x_ = 0, touch_y_ = 0;
  bool quit_ = false;
  bool lid_ = false, mic_key_ = false, mic_pad_ = false, ff_key_ = false, ff_pad_ = false;
  u32  noise_ = 0x2545F491;
  SDL_GameController* pad_ = nullptr;

  Bind key_map_[io::Io::BTN_COUNT];
  Bind pad_map_[io::Io::BTN_COUNT];
  Bind key_hot_[static_cast<int>(Action::Count)][HOT_SLOTS];
  Bind pad_hot_[static_cast<int>(Action::Count)][HOT_SLOTS];
  Bind key_mod_, pad_mod_;
  bool key_mod_down_ = false, pad_mod_down_ = false, pad_mod_used_ = false;
  int  pad_mod_button_ = -1;   // the DS button the pad modifier would otherwise be
  bool axis_state_[SDL_CONTROLLER_AXIS_MAX][2] = {};   // per axis: - and + past the threshold
  enum class StylusAxis : u8 { None, Right, Left };
  // A stick's name as the config spells it: none | left | right. The old
  // stick_dpad = true/false still parse as left/none.
  static bool parse_stick(const std::string& s, StylusAxis& out);
  StylusAxis stylus_axis_ = StylusAxis::Right;
  StylusAxis stick_dpad_ = StylusAxis::Left;   // pad.stick_dpad: a stick that also works the d-pad
  StylusAxis stick_face_ = StylusAxis::None;   // pad.stick_face: a stick that also works X/B/Y/A (up/down/left/right)
  Bind stylus_button_[HOT_SLOTS];   // pressing either touches at the pen's position (pad.stylus_button, .alt)
  u8   stylus_down_ = 0;            // which of them is held, a bit per slot
  Bind stylus_chord_;          // while held, the d-pad moves the pen instead of the game
  bool stylus_chord_down_ = false;
  u32  stylus_dpad_ = 0;       // d-pad directions held under the chord (bit per DS button)
  u32  held_ = 0;              // SDL pad buttons currently down (bit per button)
  int  deadzone_ = 12000;
  int  stylus_x_ = 0, stylus_y_ = 0;          // raw stick
  double stylus_fx_ = 128, stylus_fy_ = 96;    // pen position, DS pixels
  double stylus_speed_ = 4.0;
  int  stylus_size_ = 2;
  int  stylus_hide_ = 90, stylus_idle_ = 1 << 30;   // frames without movement or a touch; starts hidden
  std::vector<Action> actions_;
  bool capturing_ = false, capture_pad_ = false;
  std::string captured_;
  // Swallows the release of whatever was captured, so letting go of the key
  // does not immediately register as the next thing the page asked for.
  bool capture_swallow_ = false;
  bool capture_mod_ = false;      // the modifier is held: what follows is a chord
};

} // namespace ds::sdl
