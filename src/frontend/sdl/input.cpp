// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "config.h"
#include "display.h"
#include "core/nds.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace ds::sdl {

using B = io::Io::Button;

namespace {

const char* const kButtonNames[B::BTN_COUNT] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
// Keyboard defaults follow the usual emulator layout (arrows + ZX/AS).
const char* const kKeyDefaults[B::BTN_COUNT] = {"x", "z", "Right Shift", "Return", "Right", "Left", "Up", "Down", "w", "q", "s", "a"};
// SDL's controller buttons are named by position, so the DS's B (bottom) is
// SDL's A and the DS's A (right) is SDL's B.
const char* const kPadDefaults[B::BTN_COUNT] = {"b", "a", "back", "start", "dpright", "dpleft", "dpup", "dpdown", "rightshoulder", "leftshoulder", "y", "x"};

const char* const kActionNames[static_cast<int>(Action::Count)] = {
  "quit", "pause", "fast_forward", "fast_forward_toggle", "save_state", "load_state", "slot_next", "slot_prev",
  "volume_up", "volume_down", "mute", "layout_next", "fullscreen", "screenshot", "lid", "mic"};
const char* const kKeyHotDefaults[static_cast<int>(Action::Count)] = {
  "Escape", "p", "Tab", "none", "F5", "F7", "F3", "F2", "=", "-", "0", "F4", "f", "F9", "l", "m"};
const char* const kPadHotDefaults[static_cast<int>(Action::Count)] = {
  "mod+start", "mod+leftshoulder", "mod+rightshoulder", "none", "mod+b", "mod+a", "mod+x", "mod+y",
  "mod+dpup", "mod+dpdown", "none", "mod+dpright", "none", "mod+dpleft", "none", "+righttrigger"};

// Held actions: an edge on both press and release.
bool is_hold(Action a) { return a == Action::FastForward || a == Action::Mic; }

} // namespace

const char* action_name(Action a) { return kActionNames[static_cast<int>(a)]; }

Input::Bind Input::parse_key(const std::string& s0) {
  Bind b;
  std::string s = s0;
  if (s.compare(0, 4, "mod+") == 0) { b.mod = true; s = s.substr(4); }
  if (s.empty() || s == "none") return b;
  const SDL_Keycode k = SDL_GetKeyFromName(s.c_str());
  if (k == SDLK_UNKNOWN) { std::fprintf(stderr, "config: unknown key \"%s\"\n", s0.c_str()); return b; }
  b.kind = Bind::Key; b.code = k;
  return b;
}

Input::Bind Input::parse_pad(const std::string& s0) {
  Bind b;
  std::string s = s0;
  if (s.compare(0, 4, "mod+") == 0) { b.mod = true; s = s.substr(4); }
  if (s.empty() || s == "none") return b;
  if (s[0] == '+' || s[0] == '-') {
    const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(s.c_str() + 1);
    if (a == SDL_CONTROLLER_AXIS_INVALID) { std::fprintf(stderr, "config: unknown axis \"%s\"\n", s0.c_str()); return b; }
    b.kind = Bind::PadAxis; b.code = a; b.neg = s[0] == '-';
    return b;
  }
  const SDL_GameControllerButton bt = SDL_GameControllerGetButtonFromString(s.c_str());
  if (bt == SDL_CONTROLLER_BUTTON_INVALID) { std::fprintf(stderr, "config: unknown controller button \"%s\"\n", s0.c_str()); return b; }
  b.kind = Bind::PadButton; b.code = bt;
  return b;
}

void Input::configure(const Config& cfg) {
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i) {
    key_map_[i] = parse_key(cfg.str(std::string("keys.") + kButtonNames[i], kKeyDefaults[i]));
    pad_map_[i] = parse_pad(cfg.str(std::string("pad.") + kButtonNames[i], kPadDefaults[i]));
  }
  for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
    key_hot_[i] = parse_key(cfg.str(std::string("hotkeys.") + kActionNames[i], kKeyHotDefaults[i]));
    pad_hot_[i] = parse_pad(cfg.str(std::string("padhotkeys.") + kActionNames[i], kPadHotDefaults[i]));
  }
  key_mod_ = parse_key(cfg.str("hotkeys.modifier", "none"));
  pad_mod_ = parse_pad(cfg.str("padhotkeys.modifier", "back"));
  // Which DS button the pad modifier doubles as, so it can be delivered as a
  // tap when released alone.
  pad_mod_button_ = -1;
  if (pad_mod_.kind == Bind::PadButton)
    for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
      if (pad_map_[i].kind == Bind::PadButton && pad_map_[i].code == pad_mod_.code) pad_mod_button_ = i;
  stick_dpad_ = cfg.flag("pad.stick_dpad", true);
  stylus_stick_ = cfg.flag("pad.stylus_stick", false);
  deadzone_ = cfg.num("pad.stick_deadzone", 12000);
}

void Input::open_controllers() {
  for (int i = 0; i < SDL_NumJoysticks() && !pad_; ++i) {
    if (!SDL_IsGameController(i)) continue;
    pad_ = SDL_GameControllerOpen(i);
    if (pad_) std::fprintf(stderr, "controller: %s\n", SDL_GameControllerName(pad_));
  }
}

void Input::close() {
  if (pad_) { SDL_GameControllerClose(pad_); pad_ = nullptr; }
}

void Input::fake_mic_frame(std::vector<s16>& out) {
  out.resize(spu::Spu::SAMPLE_RATE / 60 + 1);
  for (s16& v : out) {                          // xorshift white noise, +/-80 % of full scale
    noise_ ^= noise_ << 13; noise_ ^= noise_ >> 17; noise_ ^= noise_ << 5;
    v = static_cast<s16>((static_cast<int>(noise_ & 0xFFFF) - 0x8000) * 4 / 5);
  }
}

void Input::touch_at(int wx, int wy, Display& display) {
  int screen = 0, sx = 0, sy = 0;
  if (!display.map_point(wx, wy, screen, sx, sy) || screen != 1) return;   // bottom screen only
  touching_ = true; touched_ = true; touch_x_ = sx; touch_y_ = sy;
}

void Input::fire(Action a, bool down) {
  if (a == Action::Quit && down) { quit_ = true; return; }
  if (is_hold(a)) return;   // the caller tracks held state
  if (down) actions_.push_back(a);
}

// Keyboard: hotkeys first (with the modifier when bound), then DS buttons.
bool Input::key_down(SDL_Keycode k, bool down) {
  if (key_mod_.kind == Bind::Key && key_mod_.code == k) { key_mod_down_ = down; return true; }
  for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
    const Bind& b = key_hot_[i];
    if (b.kind != Bind::Key || b.code != k || (b.mod && !key_mod_down_)) continue;
    const Action a = static_cast<Action>(i);
    if (a == Action::FastForward) ff_key_ = down;
    else if (a == Action::Mic) mic_key_ = down;
    else fire(a, down);
    return true;
  }
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
    if (key_map_[i].kind == Bind::Key && key_map_[i].code == k) { set(static_cast<B>(i), down); return true; }
  return false;
}

// Controller: `b` is the button or axis edge that just changed.
bool Input::pad_down(const Bind& b, bool down) {
  auto same = [&](const Bind& x) { return x.kind == b.kind && x.code == b.code && (x.kind != Bind::PadAxis || x.neg == b.neg); };
  if (same(pad_mod_)) {
    if (down) { pad_mod_down_ = true; pad_mod_used_ = false; }
    else {
      pad_mod_down_ = false;
      // Released alone: the game gets the button it doubles as, for one frame.
      if (!pad_mod_used_ && pad_mod_button_ >= 0) pressed_ |= 1u << pad_mod_button_;
    }
    return true;
  }
  for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
    const Bind& h = pad_hot_[i];
    if (!same(h) || (h.mod && !pad_mod_down_)) continue;
    if (h.mod && down) pad_mod_used_ = true;
    const Action a = static_cast<Action>(i);
    if (a == Action::FastForward) ff_pad_ = down;
    else if (a == Action::Mic) mic_pad_ = down;
    else fire(a, down);
    return true;
  }
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
    if (same(pad_map_[i])) { set(static_cast<B>(i), down); return true; }
  return false;
}

void Input::axis(Uint8 which, Sint16 value) {
  // Axis directions bound as buttons: edge detection with a threshold.
  for (int dir = 0; dir < 2; ++dir) {
    const bool past = dir ? value > deadzone_ : value < -deadzone_;
    if (past != axis_state_[which][dir]) {
      axis_state_[which][dir] = past;
      Bind b; b.kind = Bind::PadAxis; b.code = which; b.neg = dir == 0;
      pad_down(b, past);
    }
  }
  if (stick_dpad_ && (which == SDL_CONTROLLER_AXIS_LEFTX || which == SDL_CONTROLLER_AXIS_LEFTY)) {
    const B neg = which == SDL_CONTROLLER_AXIS_LEFTX ? B::BTN_LEFT : B::BTN_UP;
    const B pos = which == SDL_CONTROLLER_AXIS_LEFTX ? B::BTN_RIGHT : B::BTN_DOWN;
    stick_ &= ~((1u << neg) | (1u << pos));
    if (value < -deadzone_) stick_ |= 1u << neg;
    else if (value > deadzone_) stick_ |= 1u << pos;
  }
  if (stylus_stick_ && (which == SDL_CONTROLLER_AXIS_RIGHTX || which == SDL_CONTROLLER_AXIS_RIGHTY)) {
    // Absolute: the stick's deflection is a point on the bottom screen.
    if (which == SDL_CONTROLLER_AXIS_RIGHTX) stylus_x_ = value; else stylus_y_ = value;
    const long r2 = static_cast<long>(stylus_x_) * stylus_x_ + static_cast<long>(stylus_y_) * stylus_y_;
    stylus_down_ = r2 > static_cast<long>(deadzone_) * deadzone_;
    if (stylus_down_) {
      touch_x_ = 128 + stylus_x_ * 127 / 32767;
      touch_y_ = 96 + stylus_y_ * 95 / 32767;
    }
  }
}

void Input::handle(const SDL_Event& e, Display& display, Display* second) {
  // Route window-addressed events to the window they happened on; keyboard
  // and controller input is global.
  auto owner = [&](u32 wid) -> Display& {
    return (second && wid == second->window_id()) ? *second : display;
  };
  switch (e.type) {
  case SDL_QUIT: quit_ = true; break;

  case SDL_KEYDOWN:
  case SDL_KEYUP:
    if (e.key.repeat) break;
    key_down(e.key.keysym.sym, e.type == SDL_KEYDOWN);
    break;

  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    Bind b; b.kind = Bind::PadButton; b.code = e.cbutton.button;
    pad_down(b, e.type == SDL_CONTROLLERBUTTONDOWN);
    break;
  }
  case SDL_CONTROLLERAXISMOTION:
    if (e.caxis.axis < SDL_CONTROLLER_AXIS_MAX) axis(e.caxis.axis, e.caxis.value);
    break;

  case SDL_CONTROLLERDEVICEADDED:
    if (!pad_) open_controllers();
    break;
  case SDL_CONTROLLERDEVICEREMOVED:
    if (pad_ && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_))) {
      SDL_GameControllerClose(pad_); pad_ = nullptr;
    }
    break;

  case SDL_MOUSEBUTTONDOWN:
    if (e.button.button == SDL_BUTTON_LEFT) touch_at(e.button.x, e.button.y, owner(e.button.windowID));
    break;
  case SDL_MOUSEMOTION:
    if (e.motion.state & SDL_BUTTON_LMASK) touch_at(e.motion.x, e.motion.y, owner(e.motion.windowID));
    break;
  case SDL_MOUSEBUTTONUP:
    if (e.button.button == SDL_BUTTON_LEFT) touching_ = false;
    break;

  // The handhelds have a real touchscreen; SDL reports it in normalised
  // window coordinates.
  case SDL_FINGERDOWN:
  case SDL_FINGERMOTION: {
    Display& d = owner(e.tfinger.windowID);
    int w = 0, h = 0;
    d.output_size(w, h);
    touch_at(static_cast<int>(e.tfinger.x * w), static_cast<int>(e.tfinger.y * h), d);
    break;
  }
  case SDL_FINGERUP: touching_ = false; break;

  case SDL_WINDOWEVENT:
    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) owner(e.window.windowID).on_resize();
    else if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit_ = true;
    break;

  default: break;
  }
}

} // namespace ds::sdl
