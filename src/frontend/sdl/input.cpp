// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "config.h"
#include "display.h"
#include "core/nds.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
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

// The controls that are not a DS button and not a hotkey: the hotkey modifier
// and the pad-driven pen. The Controls page shows them as rows of their own,
// so their defaults have to be readable from there as well as from
// configure() -- one copy, or the page will offer to reset a control to
// something the emulator does not actually start with.
const char* const kModDefaultKey = "none";       // hotkeys.modifier
const char* const kModDefaultPad = "guide";      // padhotkeys.modifier: the pad's mode/home button
const char* const kStylusButtonDefault = "rightstick";
const char* const kStylusAxisDefault = "right";
const char* const kStylusDpadDefault = "none";
const char* const kStickFaceDefault = "none";
const char* const kStickDpadDefault = "left";

// Held actions: an edge on both press and release.
bool is_hold(Action a) { return a == Action::FastForward || a == Action::Mic; }

// How a controller control is spelled in the config file, and how the menu
// says it. One table for both directions: the aliases a player may write and
// the label the page draws are the same set of names, so they cannot drift.
//
// SDL names the face buttons by their Xbox letters, which are in different
// places on the pads this runs on -- so the letter is not shown at all. The
// menu draws the position pip and names the position, and the file accepts
// those same compass names as the unambiguous spelling. `sdl` is what is
// always written back, so a captured binding and a hand-written one land on
// the same line in the file.
struct PadName {
  const char* sdl;      // canonical, as SDL spells it (what we store)
  const char* label;    // what the menu draws
  const char* alias1;   // extra spellings accepted from the config file
  const char* alias2;
};
const PadName kPadNames[] = {
  {"a",             "\x01 SOUTH", "south", nullptr},
  {"b",             "\x02 EAST",  "east",  nullptr},
  {"x",             "\x03 WEST",  "west",  nullptr},
  {"y",             "\x04 NORTH", "north", nullptr},
  {"back",          "SELECT",     "select", nullptr},
  {"guide",         "FUNC BTN",   "func",  "funcbtn"},
  {"start",         "START",      nullptr, nullptr},
  {"leftshoulder",  "L1",         "l1",    nullptr},
  {"rightshoulder", "R1",         "r1",    nullptr},
  {"+lefttrigger",  "L2",         "l2",    nullptr},
  {"+righttrigger", "R2",         "r2",    nullptr},
  {"leftstick",     "L3",         "l3",    nullptr},
  {"rightstick",    "R3",         "r3",    nullptr},
  {"dpup",          "UP",         nullptr, nullptr},
  {"dpdown",        "DOWN",       nullptr, nullptr},
  {"dpleft",        "LEFT",       nullptr, nullptr},
  {"dpright",       "RIGHT",      nullptr, nullptr},
};

// One token of a binding -- no "mod+", no chord, but the axis sign is part of
// the name ("+lefttrigger" is L2, "-lefttrigger" is not a thing any pad has).
std::string canon_pad_token(const std::string& s) {
  for (const PadName& p : kPadNames) {
    if (s == p.sdl) return s;
    if ((p.alias1 && s == p.alias1) || (p.alias2 && s == p.alias2)) return p.sdl;
  }
  return s;   // not an alias: hand it to SDL unchanged, errors and all
}

const char* pad_token_label(const std::string& s) {
  for (const PadName& p : kPadNames)
    if (s == p.sdl) return p.label;
  return nullptr;
}

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
    const SDL_GameControllerButton w = SDL_GameControllerGetButtonFromString(canon_pad_token(s.substr(plus + 1)).c_str());
    if (w == SDL_CONTROLLER_BUTTON_INVALID) { std::fprintf(stderr, "config: unknown controller button in \"%s\"\n", s0.c_str()); return b; }
    b.with = w;
    s = s.substr(0, plus);
  }
  // After the split, so an alias that expands to an axis ("l2" -> "+lefttrigger")
  // still meets the sign test below.
  s = canon_pad_token(s);
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
  key_mod_ = parse_key(cfg.str("hotkeys.modifier", kModDefaultKey));
  pad_mod_ = parse_pad(cfg.str("padhotkeys.modifier", kModDefaultPad));   // BTN_MODE
  stylus_button_[0] = parse_pad(cfg.str("pad.stylus_button", kStylusButtonDefault));
  stylus_button_[1] = parse_pad(cfg.str("pad.stylus_button.alt", "none"));   // no default, like a hotkey's .alt
  stylus_speed_ = cfg.real("pad.stylus_speed", 4.0);
  stylus_size_ = cfg.num("pad.stylus_size", 2);
  stylus_hide_ = cfg.num("pad.stylus_hide", 90);
  // Which DS button the pad modifier doubles as, so it can be delivered as a
  // tap when released alone.
  pad_mod_button_ = -1;
  if (pad_mod_.kind == Bind::PadButton)
    for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i)
      if (pad_map_[i].kind == Bind::PadButton && pad_map_[i].code == pad_mod_.code) pad_mod_button_ = i;
  {
    // stylus_axis: right (default) | left | none; stylus_stick = false is the old spelling of none.
    const std::string ax = cfg.str("pad.stylus_axis", stylus_axis_default(cfg));
    if (ax == "right") stylus_axis_ = StylusAxis::Right;
    else if (ax == "left") stylus_axis_ = StylusAxis::Left;
    else if (ax == "none") stylus_axis_ = StylusAxis::None;
    else { std::fprintf(stderr, "config: stylus_axis \"%s\" is not right | left | none\n", ax.c_str()); stylus_axis_ = StylusAxis::Right; }
  }
  stylus_chord_ = parse_pad(cfg.str("pad.stylus_dpad", kStylusDpadDefault));
  {
    // stick_dpad: left (default) | right | none, a stick that also works the
    // d-pad; stick_face: none (default) | left | right, one whose directions
    // are X (up), B (down), Y (left) and A (right), the DS's own layout.
    const std::string sd = cfg.str("pad.stick_dpad", kStickDpadDefault), sf = cfg.str("pad.stick_face", kStickFaceDefault);
    if (!parse_stick(sd, stick_dpad_)) { std::fprintf(stderr, "config: stick_dpad \"%s\" is not none | left | right\n", sd.c_str()); stick_dpad_ = StylusAxis::Left; }
    if (!parse_stick(sf, stick_face_)) { std::fprintf(stderr, "config: stick_face \"%s\" is not none | left | right\n", sf.c_str()); stick_face_ = StylusAxis::None; }
    if (stick_dpad_ != StylusAxis::None && stick_dpad_ == stylus_axis_)
      std::fprintf(stderr, "config: pad.stick_dpad and pad.stylus_axis are both the %s stick; the pen keeps it\n", sd.c_str());
    if (stick_face_ != StylusAxis::None && stick_face_ == stylus_axis_)
      std::fprintf(stderr, "config: pad.stick_face and pad.stylus_axis are both the %s stick; the pen keeps it\n", sf.c_str());
    if (stick_face_ != StylusAxis::None && stick_face_ == stick_dpad_ && stick_face_ != stylus_axis_)
      std::fprintf(stderr, "config: pad.stick_dpad and pad.stick_face are both the %s stick; it will work both the d-pad and X/B/Y/A\n", sf.c_str());
  }
  deadzone_ = cfg.num("pad.stick_deadzone", 12000);
  stick_ = stick_prev_ = 0;   // a stick held across a reconfigure re-asserts itself on its next motion
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
    for (int sl = 0; sl < HOT_SLOTS; ++sl)
      if (stylus_visible_binding() && same(stylus_button_[sl], pad_map_[i]))
        std::fprintf(stderr, "config: pad.stylus_button%s = %s shadows pad.%s; the game will never see it\n", hot_suffix(sl), pad_name(stylus_button_[sl]).c_str(), kButtonNames[i]);
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
      for (int tb = 0; tb < HOT_SLOTS; ++tb)
        if (stylus_visible_binding() && same(h, stylus_button_[tb]))
          std::fprintf(stderr, "config: padhotkeys.%s%s = %s is pad.stylus_button%s; bind it as mod+%s or it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str(), hot_suffix(tb), pad_name(h).c_str());
      if (same(h, stylus_chord_))
        std::fprintf(stderr, "config: padhotkeys.%s%s = %s is pad.stylus_dpad; bind it as mod+%s or it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str(), pad_name(h).c_str());
    }
    if (same(h, pad_mod_) && h.with < 0)
      std::fprintf(stderr, "config: padhotkeys.%s%s = %s is the modifier; it will never fire\n", kActionNames[a], hot_suffix(sl), pad_name(h).c_str());
  }
  for (int sl = 0; sl < HOT_SLOTS; ++sl)
    if (stylus_visible_binding() && same(stylus_button_[sl], stylus_chord_))
      std::fprintf(stderr, "config: pad.stylus_button%s and pad.stylus_dpad are both %s; the chord will never engage\n", hot_suffix(sl), pad_name(stylus_button_[sl]).c_str());
  if (stylus_visible_binding() && exact(stylus_button_[0], stylus_button_[1]))
    std::fprintf(stderr, "config: pad.stylus_button.alt = %s is already pad.stylus_button\n", pad_name(stylus_button_[1]).c_str());
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

// The menu's fallback controls (input.h). Both tables are consulted only for a
// control nothing is bound to, so they add menu actions and never duplicate
// one.
//
// The pad table follows the console rather than SDL's letters: on a DS the A
// button is the one on the right, which is what the shipped default puts DS A
// on (pad.a = b), so east confirms and south cancels. A player who has swapped
// them has both bound and sees none of this.
void Input::menu_fallback_pad(const Bind& b, bool down) {
  if (b.kind != Bind::PadButton) return;   // buttons only: an axis is nobody's idea of a menu key
  switch (b.code) {
  case SDL_CONTROLLER_BUTTON_B:         menu_fallback(B::BTN_A, down); break;      // east
  case SDL_CONTROLLER_BUTTON_A:         menu_fallback(B::BTN_B, down); break;      // south
  case SDL_CONTROLLER_BUTTON_START:     menu_fallback(B::BTN_START, down); break;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:   menu_fallback(B::BTN_UP, down); break;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: menu_fallback(B::BTN_DOWN, down); break;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: menu_fallback(B::BTN_LEFT, down); break;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:menu_fallback(B::BTN_RIGHT, down); break;
  default: break;
  }
}

void Input::menu_fallback_key(SDL_Keycode k, bool down) {
  switch (k) {
  case SDLK_RETURN: case SDLK_KP_ENTER: menu_fallback(B::BTN_A, down); break;
  // Escape is the quit hotkey by default, so it is bound and never arrives
  // here; it only becomes the menu's cancel for a player who took quit off it.
  case SDLK_ESCAPE:    menu_fallback(B::BTN_B, down); break;
  case SDLK_UP:        menu_fallback(B::BTN_UP, down); break;
  case SDLK_DOWN:      menu_fallback(B::BTN_DOWN, down); break;
  case SDLK_LEFT:      menu_fallback(B::BTN_LEFT, down); break;
  case SDLK_RIGHT:     menu_fallback(B::BTN_RIGHT, down); break;
  default: break;
  }
}

// Controller: `b` is the button or axis edge that just changed.
bool Input::pad_down(const Bind& b, bool down) {
  auto same = [&](const Bind& x) { return x.kind == b.kind && x.code == b.code && (x.kind != Bind::PadAxis || x.neg == b.neg); };
  if (b.kind == Bind::PadButton) { if (down) held_ |= 1u << b.code; else held_ &= ~(1u << b.code); }
  // The pen's tap button and d-pad chord come first, but yield to the pad
  // modifier: mod+<tap button> is free to be a hotkey, and plain presses
  // still tap. A release always ends a tap or chord that is in progress,
  // even if the modifier was pressed in between.
  for (int sl = 0; sl < HOT_SLOTS; ++sl) {
    const u8 bit = static_cast<u8>(1u << sl);
    if (stylus_visible_binding() && same(stylus_button_[sl]) && ((stylus_down_ & bit) || (down && !pad_mod_down_))) {
      if (down) { stylus_down_ |= bit; touched_ = true; } else stylus_down_ &= static_cast<u8>(~bit);
      return true;
    }
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
  const bool left_x = which == SDL_CONTROLLER_AXIS_LEFTX, left_y = which == SDL_CONTROLLER_AXIS_LEFTY;
  const bool right_x = which == SDL_CONTROLLER_AXIS_RIGHTX, right_y = which == SDL_CONTROLLER_AXIS_RIGHTY;
  const bool x_axis = left_x || right_x;
  // Is this axis on the stick a setting names, and is that stick not the pen's?
  const auto on_stick = [&](StylusAxis s) {
    return s != stylus_axis_ && (s == StylusAxis::Left ? (left_x || left_y) : s == StylusAxis::Right ? (right_x || right_y) : false);
  };
  // A stick as the d-pad, and a stick as the face buttons laid out as the DS
  // has them: X up, B down, Y left, A right. The pen's stick stays the pen's.
  if (on_stick(stick_dpad_)) stick_as_buttons(value, x_axis ? B::BTN_LEFT : B::BTN_UP, x_axis ? B::BTN_RIGHT : B::BTN_DOWN);
  if (on_stick(stick_face_)) stick_as_buttons(value, x_axis ? B::BTN_Y : B::BTN_X, x_axis ? B::BTN_A : B::BTN_B);
  const Uint8 px = pen_left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX, py = pen_left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY;
  if (stylus_axis_ != StylusAxis::None && (which == px || which == py)) {
    if (which == px) stylus_x_ = value; else stylus_y_ = value;   // integrated by update_stylus()
  }
}

void Input::stick_as_buttons(Sint16 value, B neg, B pos) {
  stick_ &= ~((1u << neg) | (1u << pos));
  if (value < -deadzone_) stick_ |= 1u << neg;
  else if (value > deadzone_) stick_ |= 1u << pos;
  stick_pressed_ |= stick_ & ~stick_prev_;   // edges, for the pause menu
  stick_prev_ = stick_;
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

// A whole binding as the menu should draw it: "mod+leftshoulder" is "MOD+L1",
// "mod+start+back" is "MOD+START+SELECT". Anything with no entry falls back to
// upper case, which is what the page did for every name before.
std::string Input::pad_label(const std::string& s0) {
  std::string s = s0, out;
  if (s.compare(0, 4, "mod+") == 0) { out = "MOD+"; s = s.substr(4); }
  if (s.empty() || s == "none" || s == "null") return out.empty() ? std::string("NONE") : out + "NONE";
  std::string head = s, with;
  if (const size_t plus = s.find('+', 1); plus != std::string::npos && s[0] != '+' && s[0] != '-') {
    head = s.substr(0, plus);
    with = s.substr(plus + 1);
  }
  auto one = [](const std::string& t) {
    const std::string c = canon_pad_token(t);
    if (const char* l = pad_token_label(c)) return std::string(l);
    std::string u = c;
    for (char& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return u;
  };
  out += one(head);
  if (!with.empty()) out += "+" + one(with);
  return out;
}

const char* Input::mod_default(bool pad) { return pad ? kModDefaultPad : kModDefaultKey; }
const char* Input::stylus_button_default() { return kStylusButtonDefault; }
const char* Input::stylus_dpad_default() { return kStylusDpadDefault; }
const char* Input::stick_face_default() { return kStickFaceDefault; }
const char* Input::stick_dpad_default() { return kStickDpadDefault; }
bool Input::parse_stick(const std::string& s, StylusAxis& out) {
  if (s == "none" || s == "false" || s == "0") out = StylusAxis::None;
  else if (s == "left" || s == "true" || s == "1") out = StylusAxis::Left;   // true: the left stick, as it always was
  else if (s == "right") out = StylusAxis::Right;
  else return false;
  return true;
}
// "pad.stylus_stick = false" is the old spelling of "none", and a config file
// written before the axis key existed still says it, so the default the page
// shows has to come through the same fallback configure() uses.
const char* Input::stylus_axis_default(const Config& cfg) {
  return cfg.flag("pad.stylus_stick", true) ? kStylusAxisDefault : "none";
}

// Which stick a captured axis belongs to, as the generic name the config file
// wants: the pen follows a stick, not one of its two axes, so whichever way
// the player pushed it is the same answer.
const char* Input::stylus_axis_of(const std::string& captured) {
  const std::string a = captured.size() > 1 && (captured[0] == '+' || captured[0] == '-') ? captured.substr(1) : captured;
  if (a == "leftx" || a == "lefty") return "left";
  if (a == "rightx" || a == "righty") return "right";
  return nullptr;   // not a stick: a trigger or a button says nothing about which
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
  capture_mod_ = false;
}

std::string Input::take_capture() {
  if (captured_.empty()) return "";
  std::string out;
  out.swap(captured_);
  capturing_ = false;
  return out;
}

// A stick at rest still reports small motion, and a trigger may rest at its
// minimum or centred (see axis()), so "the player moved this" needs the same
// raised bar in the capture as it does in play.
bool Input::axis_moved(int axis, int value) const {
  const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
  return std::abs(value) >= (trigger ? std::max(deadzone_, 24576) : std::max(deadzone_, 16384));
}

bool Input::is_pad_mod_button(int sdl_button) const {
  return pad_mod_.kind == Bind::PadButton && pad_mod_.code == sdl_button;
}
bool Input::is_key_mod(SDL_Keycode k) const { return key_mod_.kind == Bind::Key && key_mod_.code == k; }

// True when the event was swallowed. Only the device being listened to is
// taken, so pressing a key while the pad column is open does nothing rather
// than writing a key name into [pad].
bool Input::capture_event(const SDL_Event& e) {
  // The modifier is the one control that is not taken when it goes down: held,
  // it is the first half of a chord ("mod+start"), which is how most of the
  // pad hotkeys are spelled and which there was otherwise no way to enter
  // here. So it is read on the way up instead -- released with nothing pressed
  // after it, it was not a chord and it binds itself.
  if (e.type == SDL_CONTROLLERBUTTONUP && capture_pad_ && capture_mod_ && is_pad_mod_button(e.cbutton.button)) {
    capture_mod_ = false;
    if (captured_.empty()) if (const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button))) captured_ = n;
    return true;
  }
  if (e.type == SDL_KEYUP && !capture_pad_ && capture_mod_ && is_key_mod(e.key.keysym.sym)) {
    capture_mod_ = false;
    if (captured_.empty()) if (const char* n = SDL_GetKeyName(e.key.keysym.sym)) captured_ = n;
    return true;
  }
  // The release of whatever was just captured, and the release of the button
  // that opened the capture, are swallowed rather than acted on.
  if (e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONUP) return true;
  if (!captured_.empty()) return true;   // waiting to be collected
  if (!capture_pad_) {
    // Nothing on a controller can be written into [keys], and a handheld has
    // no keyboard to satisfy this column with -- so a player who opens it
    // there has nothing that will do. Anything but a key backs out of the
    // bind, and is swallowed on the way so that the button which cancelled
    // does not also act on the row it just left (A would reopen the capture
    // it had closed).
    if (e.type == SDL_CONTROLLERBUTTONDOWN) { capturing_ = false; return true; }
    if (e.type == SDL_CONTROLLERAXISMOTION) {
      if (axis_moved(e.caxis.axis, e.caxis.value)) capturing_ = false;
      return true;   // resting-stick noise is eaten either way
    }
    if (e.type != SDL_KEYDOWN || e.key.repeat) return e.type == SDL_KEYDOWN;
    // Escape cancels rather than binding itself: a page that can only be left
    // by binding something is a trap, and Escape is the quit hotkey's default.
    if (e.key.keysym.sym == SDLK_ESCAPE) { capturing_ = false; return true; }
    if (is_key_mod(e.key.keysym.sym)) { capture_mod_ = true; return true; }
    const char* n = SDL_GetKeyName(e.key.keysym.sym);
    captured_ = (capture_mod_ ? "mod+" : "") + std::string(n && *n ? n : "none");
    return true;
  }
  // A key pressed while the pad column is listening is not a name to write
  // down, but it must not act either: without this it falls through to the
  // ordinary handler and Escape -- the one key a player will try to get out of
  // here -- fires the quit hotkey and takes the emulator with it. Escape
  // cancels, as it does for the keyboard column; every other key is eaten.
  if (e.type == SDL_KEYDOWN) {
    if (!e.key.repeat && e.key.keysym.sym == SDLK_ESCAPE) capturing_ = false;
    return true;
  }
  if (e.type == SDL_CONTROLLERBUTTONDOWN) {
    if (is_pad_mod_button(e.cbutton.button)) { capture_mod_ = true; return true; }
    if (const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button)))
      captured_ = (capture_mod_ ? "mod+" : "") + std::string(n);
    return true;
  }
  if (e.type == SDL_CONTROLLERAXISMOTION) {
    const int axis = e.caxis.axis;
    if (!axis_moved(axis, e.caxis.value)) return true;
    // "mod++righttrigger" is a real spelling and a shipped default, so the
    // modifier prefixes an axis as readily as a button.
    if (const char* n = SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(axis)))
      captured_ = std::string(capture_mod_ ? "mod+" : "") + (e.caxis.value < 0 ? "-" : "+") + n;
    return true;
  }
  // Everything else (mouse, touch, window) still goes through: closing the
  // window while a capture is open must still close it.
  return false;
}

// Every binding that shadows another, as one line each. warn_collisions()
// prints the same at startup; this is for the player who is making one.
// Can this DS button still be pressed at all? The menu is driven by these
// bindings, so a DS A bound to nothing -- or to a control this pad does not
// physically have -- takes the Controls page itself out of reach, and with it
// the reset that would undo the mistake.
//
// A pad is judged against the pad in hand: SDL knows which buttons and axes
// the mapping actually names, so "rightstick" on a pad with no stick click is
// caught as well as "none". With no pad open the keyboard is all there is.
// Is any binding at all sitting on this pad button? A chord partner is not:
// pad_down() only consumes a partner while the chord's own button is held, so
// a lone press of it still falls through to the menu fallback.
bool Input::pad_control_free(int sdl_button) const {
  const auto on = [&](const Bind& b) { return b.kind == Bind::PadButton && b.code == sdl_button; };
  if (on(pad_mod_) || on(stylus_button_[0]) || on(stylus_button_[1]) || on(stylus_chord_)) return false;
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i) if (on(pad_map_[i])) return false;
  for (int a = 0; a < static_cast<int>(Action::Count); ++a)
    for (int sl = 0; sl < HOT_SLOTS; ++sl) if (on(pad_hot_[a][sl])) return false;
  return true;
}

bool Input::key_control_free(SDL_Keycode k) const {
  const auto on = [&](const Bind& b) { return b.kind == Bind::Key && b.code == k; };
  if (on(key_mod_)) return false;
  for (int i = 0; i < static_cast<int>(B::BTN_COUNT); ++i) if (on(key_map_[i])) return false;
  for (int a = 0; a < static_cast<int>(Action::Count); ++a)
    for (int sl = 0; sl < HOT_SLOTS; ++sl) if (on(key_hot_[a][sl])) return false;
  return true;
}

// Has this DS button a binding on a control the hardware in hand actually has?
// With a pad open the pad column is what the player is using: a keyboard
// binding on a handheld with no keyboard is not a way out.
bool Input::bound_and_present(int ds_button) const {
  const Bind& b = pad_ ? pad_map_[ds_button] : key_map_[ds_button];
  if (!pad_) return b.kind == Bind::Key;
#if SDL_VERSION_ATLEAST(2, 0, 14)
  if (b.kind == Bind::PadButton)
    return SDL_GameControllerHasButton(pad_, static_cast<SDL_GameControllerButton>(b.code)) == SDL_TRUE;
  if (b.kind == Bind::PadAxis)
    return SDL_GameControllerHasAxis(pad_, static_cast<SDL_GameControllerAxis>(b.code)) == SDL_TRUE;
  return false;
#else
  return b.kind != Bind::None;
#endif
}

bool Input::reachable(int ds_button) const {
  if (bound_and_present(ds_button)) return true;
  // The menu's own fallback counts as a way to press it: it is what rescues
  // the usual mistakes, and a warning that fires when nothing is actually
  // broken is a warning players learn to read past.
  if (ds_button == B::BTN_A && (pad_ ? pad_control_free(SDL_CONTROLLER_BUTTON_B) : key_control_free(SDLK_RETURN))) return true;
  if (ds_button == B::BTN_B && (pad_ ? pad_control_free(SDL_CONTROLLER_BUTTON_A) : key_control_free(SDLK_ESCAPE))) return true;
  // START confirms on every page but this one, so a working START is still a
  // way in to the page that resets the lot. It is not offered for B: START
  // backs out of nothing, and the way out of the menu with no B is the pause
  // hotkey, which lives in its own namespace and cannot be broken from here.
  if (ds_button == B::BTN_A)
    return bound_and_present(B::BTN_START) || (pad_ && pad_control_free(SDL_CONTROLLER_BUTTON_START));
  return false;
}

std::vector<std::string> Input::collisions() const {
  std::vector<std::string> out;
  // First, because the page shows one line and this is the one that cannot be
  // recovered from without a text editor. A and B are what the menu needs: A
  // to go in and act, B to come back out.
  for (const int i : {static_cast<int>(B::BTN_A), static_cast<int>(B::BTN_B)})
    if (!reachable(i))
      out.push_back(std::string(kButtonNames[i]) + " IS UNREACHABLE; X RESETS");
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
    if (!key_down(e.key.keysym.sym, e.type == SDL_KEYDOWN))
      menu_fallback_key(e.key.keysym.sym, e.type == SDL_KEYDOWN);
    break;

  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    Bind b; b.kind = Bind::PadButton; b.code = e.cbutton.button;
    if (!pad_down(b, e.type == SDL_CONTROLLERBUTTONDOWN))
      menu_fallback_pad(b, e.type == SDL_CONTROLLERBUTTONDOWN);
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
