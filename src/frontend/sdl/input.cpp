// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "config.h"
#include "display.h"
#include "core/nds.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
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
  "volume_up", "volume_down", "mute", "layout_next", "layout_prev", "screen_swap", "pip_corner_next", "fullscreen", "screenshot", "lid", "mic", "fps"};
const char* const kKeyHotDefaults[static_cast<int>(Action::Count)] = {
  "Escape", "p", "Tab", "none", "F5", "F7", "F3", "F2", "=", "-", "0", "F4", "F10", "F6", "F8", "f", "F9", "l", "m", "none"};
const char* const kPadHotDefaults[static_cast<int>(Action::Count)] = {
  "mod+start+back", "mod+start", "mod++righttrigger", "none", "mod+rightshoulder", "mod+leftshoulder", "mod+dpright", "mod+dpleft",
  "none", "none", "none", "mod+back", "mod+x", "mod+y", "none", "none", "none", "none", "leftstick", "none"};

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
  if (s.empty() || s == "none" || s == "null") return b;
  // A chord: "start+select" is start with select held (either order works
  // for the player; the last pressed completes it).
  if (const size_t plus = s.find('+', 1); plus != std::string::npos && s[0] != '+' && s[0] != '-') {
    const SDL_GameControllerButton w = SDL_GameControllerGetButtonFromString(s.substr(plus + 1).c_str());
    if (w == SDL_CONTROLLER_BUTTON_INVALID) { std::fprintf(stderr, "config: unknown controller button in \"%s\"\n", s0.c_str()); return b; }
    b.with = w;
    s = s.substr(0, plus);
  }
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
    key_hot_[i][0] = parse_key(cfg.str(std::string("hotkeys.") + kActionNames[i], kKeyHotDefaults[i]));
    pad_hot_[i][0] = parse_pad(cfg.str(std::string("padhotkeys.") + kActionNames[i], kPadHotDefaults[i]));
    // The second binding, if the player set one. No default: unset means the
    // action has the one control its column's layout gives it.
    key_hot_[i][1] = parse_key(cfg.str(std::string("hotkeys.") + kActionNames[i] + ".alt", "none"));
    pad_hot_[i][1] = parse_pad(cfg.str(std::string("padhotkeys.") + kActionNames[i] + ".alt", "none"));
  }
  key_mod_ = parse_key(cfg.str("hotkeys.modifier", "none"));
  pad_mod_ = parse_pad(cfg.str("padhotkeys.modifier", "guide"));   // BTN_MODE
  stylus_button_ = parse_pad(cfg.str("pad.stylus_button", "rightstick"));
  stylus_speed_ = cfg.real("pad.stylus_speed", 4.0);
  stylus_size_ = cfg.num("pad.stylus_size", 2);
  stylus_hide_ = cfg.num("pad.stylus_hide", 90);
  // Which DS button the pad modifier doubles as, so it can be delivered as a
  // tap when released alone.
  pad_mod_button_ = -1;
  if (pad_mod_.kind == Bind::PadButton)
    for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
      if (pad_map_[i].kind == Bind::PadButton && pad_map_[i].code == pad_mod_.code) pad_mod_button_ = i;
  stick_dpad_ = cfg.flag("pad.stick_dpad", true);
  {
    // stylus_axis: right (default) | left | none; stylus_stick = false is the old spelling of none.
    const std::string ax = cfg.str("pad.stylus_axis", cfg.flag("pad.stylus_stick", true) ? "right" : "none");
    if (ax == "right") stylus_axis_ = StylusAxis::Right;
    else if (ax == "left") stylus_axis_ = StylusAxis::Left;
    else if (ax == "none") stylus_axis_ = StylusAxis::None;
    else { std::fprintf(stderr, "config: stylus_axis \"%s\" is not right | left | none\n", ax.c_str()); stylus_axis_ = StylusAxis::Right; }
  }
  stylus_chord_ = parse_pad(cfg.str("pad.stylus_dpad", "none"));
  deadzone_ = cfg.num("pad.stick_deadzone", 12000);
  warn_collisions();
}

// A physical control feeds one binding per edge: the first match in
// pad_down()'s / key_down()'s order wins and the rest never fire. Say so at
// launch rather than letting the player discover a dead button.
void Input::warn_collisions() const {
  auto same = [](const Bind& x, const Bind& y) {
    return x.kind != Bind::None && x.kind == y.kind && x.code == y.code && (x.kind != Bind::PadAxis || x.neg == y.neg);
  };
  auto exact = [&](const Bind& x, const Bind& y) { return same(x, y) && x.mod == y.mod && x.with == y.with; };
  auto pad_name = [](const Bind& b) {
    std::string s = b.mod ? "mod+" : "";
    if (b.kind == Bind::PadAxis) s += (b.neg ? "-" : "+") + std::string(SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(b.code)));
    else s += SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b.code));
    if (b.with >= 0) s += std::string("+") + SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b.with));
    return s;
  };
  auto key_name = [](const Bind& b) { return std::string(b.mod ? "mod+" : "") + SDL_GetKeyName(b.code); };
  const int nb = static_cast<int>(B::BTN_COUNT), na = static_cast<int>(Action::Count);

  // Keyboard.
  for (int i = 0; i < nb; ++i) {
    for (int j = 0; j < i; ++j)
      if (exact(key_map_[i], key_map_[j]))
        std::fprintf(stderr, "config: keys.%s = %s is already keys.%s; only %s will fire\n", kButtonNames[i], key_name(key_map_[i]).c_str(), kButtonNames[j], kButtonNames[j]);
    for (int a = 0; a < na; ++a)
      for (int sl = 0; sl < HOT_SLOTS; ++sl)
        if (same(key_hot_[a][sl], key_map_[i]) && !key_hot_[a][sl].mod)
          std::fprintf(stderr, "config: hotkeys.%s%s = %s shadows keys.%s; the game will never see it\n", kActionNames[a], hot_suffix(sl), key_name(key_hot_[a][sl]).c_str(), kButtonNames[i]);
    if (same(key_mod_, key_map_[i]))
      std::fprintf(stderr, "config: hotkeys.modifier = %s shadows keys.%s\n", key_name(key_mod_).c_str(), kButtonNames[i]);
  }
  // Every binding against every earlier one, the two slots included: the
  // first match in key_down()'s order wins, and that order is action then
  // slot.
  for (int a = 0; a < na * HOT_SLOTS; ++a)
    for (int b = 0; b < a; ++b)
      if (exact(key_hot_[a / HOT_SLOTS][a % HOT_SLOTS], key_hot_[b / HOT_SLOTS][b % HOT_SLOTS]))
        std::fprintf(stderr, "config: hotkeys.%s%s = %s is already hotkeys.%s%s; only %s will fire\n",
                     kActionNames[a / HOT_SLOTS], hot_suffix(a % HOT_SLOTS), key_name(key_hot_[a / HOT_SLOTS][a % HOT_SLOTS]).c_str(),
                     kActionNames[b / HOT_SLOTS], hot_suffix(b % HOT_SLOTS), kActionNames[b / HOT_SLOTS]);

  // Controller. The modifier doubling as a DS button is by design (a lone
  // release delivers it as a tap), so that pair is not a collision.
  for (int i = 0; i < nb; ++i) {
    for (int j = 0; j < i; ++j)
      if (exact(pad_map_[i], pad_map_[j]))
        std::fprintf(stderr, "config: pad.%s = %s is already pad.%s; only %s will fire\n", kButtonNames[i], pad_name(pad_map_[i]).c_str(), kButtonNames[j], kButtonNames[j]);
    for (int a = 0; a < na; ++a)
      for (int sl = 0; sl < HOT_SLOTS; ++sl)
        if (same(pad_hot_[a][sl], pad_map_[i]) && !pad_hot_[a][sl].mod && pad_hot_[a][sl].with < 0)
          std::fprintf(stderr, "config: padhotkeys.%s%s = %s shadows pad.%s; the game will never see it\n", kActionNames[a], hot_suffix(sl), pad_name(pad_hot_[a][sl]).c_str(), kButtonNames[i]);
    if (stylus_visible_binding() && same(stylus_button_, pad_map_[i]))
      std::fprintf(stderr, "config: pad.stylus_button = %s shadows pad.%s; the game will never see it\n", pad_name(stylus_button_).c_str(), kButtonNames[i]);
    if (same(stylus_chord_, pad_map_[i]))
      std::fprintf(stderr, "config: pad.stylus_dpad = %s shadows pad.%s; the game will never see it\n", pad_name(stylus_chord_).c_str(), kButtonNames[i]);
  }
  for (int k = 0; k < na * HOT_SLOTS; ++k) {
    const int a = k / HOT_SLOTS, sl = k % HOT_SLOTS;
    const Bind& h = pad_hot_[a][sl];
    for (int b = 0; b < k; ++b)
      if (exact(h, pad_hot_[b / HOT_SLOTS][b % HOT_SLOTS]))
        std::fprintf(stderr, "config: padhotkeys.%s%s = %s is already padhotkeys.%s%s; only %s will fire\n",
                     kActionNames[a], hot_suffix(sl), pad_name(h).c_str(),
                     kActionNames[b / HOT_SLOTS], hot_suffix(b % HOT_SLOTS), kActionNames[b / HOT_SLOTS]);
    // The pen claims its button unless the modifier is held, so only an
    // unmodified hotkey on it is dead.
    if (!h.mod) {
      if (stylus_visible_binding() && same(h, stylus_button_))
        std::fprintf(stderr, "config: padhotkeys.%s%s = %s is pad.stylus_button; bind it as mod+%s or it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str(), pad_name(h).c_str());
      if (same(h, stylus_chord_))
        std::fprintf(stderr, "config: padhotkeys.%s%s = %s is pad.stylus_dpad; bind it as mod+%s or it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str(), pad_name(h).c_str());
    }
    if (same(h, pad_mod_) && h.with < 0)
      std::fprintf(stderr, "config: padhotkeys.%s%s = %s is the modifier; it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str());
  }
  if (stylus_visible_binding() && same(stylus_button_, stylus_chord_))
    std::fprintf(stderr, "config: pad.stylus_button and pad.stylus_dpad are both %s; the chord will never engage\n", pad_name(stylus_button_).c_str());
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
  for (int i = 0; i < static_cast<int>(Action::Count) * HOT_SLOTS; ++i) {
    const Bind& b = key_hot_[i / HOT_SLOTS][i % HOT_SLOTS];
    if (b.kind != Bind::Key || b.code != k || (b.mod && !key_mod_down_)) continue;
    const Action a = static_cast<Action>(i / HOT_SLOTS);
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
  if (b.kind == Bind::PadButton) { if (down) held_ |= 1u << b.code; else held_ &= ~(1u << b.code); }
  // The pen's tap button and d-pad chord come first, but yield to the pad
  // modifier: mod+<tap button> is free to be a hotkey, and plain presses
  // still tap. A release always ends a tap or chord that is in progress,
  // even if the modifier was pressed in between.
  if (stylus_visible_binding() && same(stylus_button_) && (stylus_down_ || (down && !pad_mod_down_))) {
    stylus_down_ = down; if (down) touched_ = true; return true;
  }
  // The d-pad chord: the chord button itself is withheld from the game, and
  // while it is held the four directions move the pen instead.
  if (same(stylus_chord_) && (stylus_chord_down_ || (down && !pad_mod_down_))) {
    stylus_chord_down_ = down;
    if (!down) stylus_dpad_ = 0;
    return true;
  }
  if (stylus_chord_down_) {
    for (const B d : {B::BTN_UP, B::BTN_DOWN, B::BTN_LEFT, B::BTN_RIGHT})
      if (same(pad_map_[d])) {
        if (down) stylus_dpad_ |= 1u << d; else stylus_dpad_ &= ~(1u << d);
        buttons_ &= ~(1u << d);   // in case it was down before the chord
        return true;
      }
  }
  if (same(pad_mod_)) {
    if (down) { pad_mod_down_ = true; pad_mod_used_ = false; }
    else {
      pad_mod_down_ = false;
      // Released alone: the game gets the button it doubles as, for one frame.
      if (!pad_mod_used_ && pad_mod_button_ >= 0) pressed_ |= 1u << pad_mod_button_;
    }
    return true;
  }
  // Hotkeys: a chord's partner may be pressed in either order, so a button
  // matches both as the binding's own button (partner held) and as the
  // partner (own button held). The most specific binding wins.
  int best = -1, best_score = -1;
  for (int i = 0; i < static_cast<int>(Action::Count) * HOT_SLOTS; ++i) {
    const Bind& h = pad_hot_[i / HOT_SLOTS][i % HOT_SLOTS];
    if (h.kind == Bind::None || (h.mod && !pad_mod_down_)) continue;
    const bool own = same(h) && (h.with < 0 || (held_ >> h.with) & 1);
    const bool partner = h.with >= 0 && b.kind == Bind::PadButton && b.code == h.with && h.kind == Bind::PadButton && ((held_ >> h.code) & 1);
    if (!own && !partner) continue;
    const int score = (h.mod ? 1 : 0) + (h.with >= 0 ? 1 : 0);
    if (score > best_score) { best = i; best_score = score; }
  }
  if (best >= 0) {
    const Bind& h = pad_hot_[best / HOT_SLOTS][best % HOT_SLOTS];
    if (h.mod && down) pad_mod_used_ = true;
    const Action a = static_cast<Action>(best / HOT_SLOTS);
    if (a == Action::FastForward) ff_pad_ = down;
    else if (a == Action::Mic) mic_pad_ = down;
    else fire(a, down);
    return true;
  }
  // A release that completes no binding still ends a held action.
  if (!down)
    for (int sl = 0; sl < HOT_SLOTS; ++sl) {
      if (same(pad_hot_[static_cast<int>(Action::FastForward)][sl])) ff_pad_ = false;
      if (same(pad_hot_[static_cast<int>(Action::Mic)][sl])) mic_pad_ = false;
    }
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
    if (same(pad_map_[i])) { set(static_cast<B>(i), down); return true; }
  return false;
}

void Input::axis(Uint8 which, Sint16 value) {
  // Axis directions bound as buttons: edge detection with a threshold.
  // A trigger is 0 at rest and 32767 pressed -- unless the pad's mapping
  // binds it to a centred axis (the Miyoo A30's "Xbox 360" pad: L2/R2 are
  // -256..256 evdev axes, idle 0), which SDL rescales to rest at 16384. A
  // stick deadzone would then see the release as still pressed, and the
  // hotkey fires exactly once. Triggers need three quarters of the travel.
  const bool trigger = which == SDL_CONTROLLER_AXIS_TRIGGERLEFT || which == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
  const int threshold = trigger ? std::max(deadzone_, 24576) : deadzone_;
  for (int dir = 0; dir < 2; ++dir) {
    const bool past = dir ? value > threshold : value < -threshold;
    if (past != axis_state_[which][dir]) {
      axis_state_[which][dir] = past;
      Bind b; b.kind = Bind::PadAxis; b.code = which; b.neg = dir == 0;
      pad_down(b, past);
    }
  }
  const bool pen_left = stylus_axis_ == StylusAxis::Left;
  if (stick_dpad_ && !pen_left && (which == SDL_CONTROLLER_AXIS_LEFTX || which == SDL_CONTROLLER_AXIS_LEFTY)) {
    const B neg = which == SDL_CONTROLLER_AXIS_LEFTX ? B::BTN_LEFT : B::BTN_UP;
    const B pos = which == SDL_CONTROLLER_AXIS_LEFTX ? B::BTN_RIGHT : B::BTN_DOWN;
    stick_ &= ~((1u << neg) | (1u << pos));
    if (value < -deadzone_) stick_ |= 1u << neg;
    else if (value > deadzone_) stick_ |= 1u << pos;
    stick_pressed_ |= stick_ & ~stick_prev_;   // edges, for the pause menu
    stick_prev_ = stick_;
  }
  const Uint8 px = pen_left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX, py = pen_left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY;
  if (stylus_axis_ != StylusAxis::None && (which == px || which == py)) {
    if (which == px) stylus_x_ = value; else stylus_y_ = value;   // integrated by update_stylus()
  }
}

bool Input::stylus_visible_binding() const { return stylus_axis_ != StylusAxis::None || stylus_chord_.kind != Bind::None; }

void Input::update_stylus() {
  if (!stylus_visible_binding()) return;
  auto axis = [&](int v) { return (v > deadzone_ || v < -deadzone_) ? static_cast<double>(v) / 32767.0 : 0.0; };
  const double ddx = ((stylus_dpad_ >> B::BTN_RIGHT) & 1) - static_cast<double>((stylus_dpad_ >> B::BTN_LEFT) & 1);
  const double ddy = ((stylus_dpad_ >> B::BTN_DOWN) & 1) - static_cast<double>((stylus_dpad_ >> B::BTN_UP) & 1);
  const double dx = (axis(stylus_x_) + ddx) * stylus_speed_, dy = (axis(stylus_y_) + ddy) * stylus_speed_;
  if (dx != 0 || dy != 0 || stylus_down_) stylus_idle_ = 0; else if (stylus_idle_ < (1 << 30)) ++stylus_idle_;
  stylus_fx_ += dx;
  stylus_fy_ += dy;
  stylus_fx_ = std::clamp(stylus_fx_, 0.0, 255.0);
  stylus_fy_ = std::clamp(stylus_fy_, 0.0, 191.0);
  if (stylus_down_) { touch_x_ = stylus_x(); touch_y_ = stylus_y(); }
}

const char* Input::button_name(int i) { return kButtonNames[i]; }
int Input::button_count() { return static_cast<int>(B::BTN_COUNT); }
const char* Input::key_default(int i) { return kKeyDefaults[i]; }
const char* Input::pad_default(int i) { return kPadDefaults[i]; }
int Input::action_count() { return static_cast<int>(Action::Count); }
const char* Input::key_hot_default(int i) { return kKeyHotDefaults[i]; }
const char* Input::pad_hot_default(int i) { return kPadHotDefaults[i]; }

void Input::begin_capture(bool pad) {
  capturing_ = true;
  capture_pad_ = pad;
  captured_.clear();
  capture_swallow_ = false;
}

std::string Input::take_capture() {
  if (captured_.empty()) return "";
  std::string out;
  out.swap(captured_);
  capturing_ = false;
  return out;
}

// True when the event was swallowed. Only the device being listened to is
// taken, so pressing a key while the pad column is open does nothing rather
// than writing a key name into [pad].
bool Input::capture_event(const SDL_Event& e) {
  // The release of whatever was just captured, and the release of the button
  // that opened the capture, are swallowed rather than acted on.
  if (e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONUP) return true;
  if (!captured_.empty()) return true;   // waiting to be collected
  if (!capture_pad_) {
    if (e.type != SDL_KEYDOWN || e.key.repeat) return e.type == SDL_KEYDOWN;
    // Escape cancels rather than binding itself: a page that can only be left
    // by binding something is a trap, and Escape is the quit hotkey's default.
    if (e.key.keysym.sym == SDLK_ESCAPE) { capturing_ = false; return true; }
    const char* n = SDL_GetKeyName(e.key.keysym.sym);
    captured_ = n && *n ? n : "none";
    return true;
  }
  if (e.type == SDL_CONTROLLERBUTTONDOWN) {
    const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button));
    if (n) captured_ = n;
    return true;
  }
  if (e.type == SDL_CONTROLLERAXISMOTION) {
    // A trigger rests at its minimum on some pads and centred on others, so
    // the threshold is the raised one the rest of the code uses for them.
    const int axis = e.caxis.axis;
    const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    const int threshold = trigger ? std::max(deadzone_, 24576) : std::max(deadzone_, 16384);
    if (std::abs(static_cast<int>(e.caxis.value)) < threshold) return true;
    if (const char* n = SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(axis)))
      captured_ = (e.caxis.value < 0 ? "-" : "+") + std::string(n);
    return true;
  }
  // Everything else (mouse, touch, window) still goes through: closing the
  // window while a capture is open must still close it.
  return false;
}

// Every binding that shadows another, as one line each. warn_collisions()
// prints the same at startup; this is for the player who is making one.
std::vector<std::string> Input::collisions() const {
  std::vector<std::string> out;
  const auto same = [](const Bind& a, const Bind& b) {
    return a.kind != Bind::None && a.kind == b.kind && a.code == b.code &&
           a.neg == b.neg && a.mod == b.mod && a.with == b.with;
  };
  // Two DS buttons on one control: the second never fires.
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
    for (int j = i + 1; j < static_cast<int>(B::BTN_COUNT); ++j) {
      if (same(key_map_[i], key_map_[j])) out.push_back(std::string(kButtonNames[i]) + " AND " + kButtonNames[j] + " SHARE A KEY");
      if (same(pad_map_[i], pad_map_[j])) out.push_back(std::string(kButtonNames[i]) + " AND " + kButtonNames[j] + " SHARE A BUTTON");
    }
  // A hotkey on the same control as a DS button: the hotkey is tried first,
  // so the button is dead.
  for (int a = 0; a < static_cast<int>(Action::Count); ++a)
    for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i) {
      for (int sl = 0; sl < HOT_SLOTS; ++sl) {
        if (same(key_hot_[a][sl], key_map_[i])) out.push_back(std::string(kActionNames[a]) + " HIDES KEY " + kButtonNames[i]);
        if (same(pad_hot_[a][sl], pad_map_[i])) out.push_back(std::string(kActionNames[a]) + " HIDES PAD " + kButtonNames[i]);
      }
    }
  return out;
}

void Input::handle(const SDL_Event& e, Display& display, Display* second) {
  // Route window-addressed events to the window they happened on; keyboard
  // and controller input is global.
  auto owner = [&](u32 wid) -> Display& {
    return (second && wid == second->window_id()) ? *second : display;
  };
  // Rebinding: the next thing pressed is a name to write down, not a control
  // to act on. It has to be taken before anything else looks at it, or
  // rebinding the quit hotkey would quit and rebinding A would press A.
  if (capturing_ && capture_event(e)) return;
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
