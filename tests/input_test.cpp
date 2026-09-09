// SPDX-License-Identifier: GPL-3.0-or-later
// The menu's way back from a broken binding.
//
// The pause menu is navigated with the player's own bindings, so a player who
// binds DS A to nothing reachable can no longer open the page that would put
// it back. Two things stop that being a trip to a text editor: a fallback
// layer over controls nothing is bound to, and a warning on the page itself.
// Both are only correct if they stay quiet when nothing is wrong, which is
// what most of this file checks.
#include "frontend/sdl/input.h"
#include "frontend/sdl/config.h"
#include "check.h"

#include <string>

namespace ds::sdl {

// The event-level entry points, which handle() reaches only with a Display in
// hand (input.h, InputTestAccess).
struct InputTestAccess {
  // A press and release of one pad button, down the path handle() uses.
  static void tap_pad(Input& in, SDL_GameControllerButton bt) {
    Input::Bind b; b.kind = Input::Bind::PadButton; b.code = bt;
    for (const bool down : {true, false})
      if (!in.pad_down(b, down)) in.menu_fallback_pad(b, down);
  }
  static void tap_key(Input& in, SDL_Keycode k) {
    for (const bool down : {true, false})
      if (!in.key_down(k, down)) in.menu_fallback_key(k, down);
  }
  static bool capture(Input& in, SDL_Keycode k) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    return in.capture_event(e);
  }
  static bool capture_key(Input& in, SDL_Keycode k, bool down) {
    SDL_Event e{}; e.type = down ? SDL_KEYDOWN : SDL_KEYUP; e.key.keysym.sym = k;
    return in.capture_event(e);
  }
  static bool capture_pad(Input& in, SDL_GameControllerButton b, bool down) {
    SDL_Event e{}; e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP; e.cbutton.button = b;
    return in.capture_event(e);
  }
  static bool capture_axis(Input& in, SDL_GameControllerAxis a, Sint16 v) {
    SDL_Event e{}; e.type = SDL_CONTROLLERAXISMOTION; e.caxis.axis = a; e.caxis.value = v;
    return in.capture_event(e);
  }
  static bool capture_other(Input& in, Uint32 type) {
    SDL_Event e{}; e.type = type;
    return in.capture_event(e);
  }
  static bool reachable(const Input& in, int b) { return in.reachable(b); }
  static bool key_free(const Input& in, SDL_Keycode k) { return in.key_control_free(k); }
  static u32 fb_pressed(const Input& in) { return in.menu_fb_pressed_; }
  static u32 fb_held(const Input& in) { return in.menu_fb_held_; }
  static bool quit(const Input& in) { return in.quit_; }
};

namespace {

using B = io::Io::Button;
using T = InputTestAccess;
constexpr u32 kA = 1u << B::BTN_A, kB = 1u << B::BTN_B;

// A configured Input, with whatever the caller wants to break.
struct Rig {
  Config cfg;
  Input in;
  Rig& set(const char* k, const char* v) { cfg.set(k, v); return *this; }
  Input& go() { in.configure(cfg); return in; }
};

// Under the shipped defaults every fallback control is already bound, so the
// layer must contribute nothing at all: a control with a job keeps it, and
// cannot fire two menu actions at once.
void test_fallback_is_inert_by_default() {
  Rig r; Input& in = r.go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);          // east: DS A, by the default map
  CHECK(in.take_menu_presses() == kA);
  CHECK(T::fb_pressed(in) == 0 && T::fb_held(in) == 0);
}

void test_fallback_rescues_an_orphaned_button() {
  Rig r; Input& in = r.set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  CHECK(in.take_menu_presses() == kA);              // east confirms again
}

// The rescue must never double up: east bound to DS B cancels, and does not
// also confirm.
void test_fallback_yields_to_a_real_binding() {
  Rig r; Input& in = r.set("pad.b", "b").set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  CHECK(in.take_menu_presses() == kB);
}

// A fallback press dies with the frame it happened on: an unbound Escape
// pressed during play must not be waiting to cancel the next menu opened.
void test_fallback_press_does_not_outlive_the_frame() {
  Rig r; Input& in = r.set("pad.a", "none").go();
  T::tap_pad(in, SDL_CONTROLLER_BUTTON_B);
  in.frame();
  CHECK(in.take_menu_presses() == 0);
}

// Escape is the quit hotkey by default, so it is bound and never reaches the
// fallback. It becomes the menu's cancel only for a player who took quit off it.
void test_escape_is_still_quit() {
  Rig r; Input& in = r.go();
  T::tap_key(in, SDLK_ESCAPE);
  CHECK(in.take_menu_presses() == 0);
  CHECK(T::quit(in));
}

void test_escape_cancels_once_quit_moves() {
  Rig r; Input& in = r.set("hotkeys.quit", "none").go();
  T::tap_key(in, SDLK_ESCAPE);
  CHECK(in.take_menu_presses() == kB);
  CHECK(!T::quit(in));
}

// Rebinding the pad column used to be a one-way door: a key event fell past
// the capture and reached the ordinary handler, so Escape -- the one key a
// player will try -- fired the quit hotkey and took the emulator with it.
void test_pad_capture_swallows_keys_and_escape_cancels() {
  Rig r; Input& in = r.go();
  in.begin_capture(true);
  CHECK(T::capture(in, SDLK_x));                    // swallowed
  CHECK(in.capturing());                            // and bound nothing
  CHECK(in.take_capture().empty());
  in.begin_capture(true);
  CHECK(T::capture(in, SDLK_ESCAPE));
  CHECK(!in.capturing());
  CHECK(!T::quit(in));
}

// The modifier is the first half of most of the shipped pad hotkeys
// ("mod+start"), so taking it the moment it goes down left no way to enter one
// from the page. Held, it prefixes what follows; released alone, it binds
// itself.
void test_capture_composes_a_chord_with_the_modifier() {
  // guide is the default padhotkeys.modifier.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(in.take_capture().empty());                     // nothing yet: it may be a chord
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_START, true));
    CHECK(in.take_capture() == "mod+start"); }
  // Released with nothing pressed after it, it was not a chord.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false));
    CHECK(in.take_capture() == "guide"); }
  // An axis takes the prefix too: "mod++righttrigger" is a shipped default.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32000));
    CHECK(in.take_capture() == "mod++righttrigger"); }
  // A pad whose modifier has been moved follows it, and the old one is then
  // an ordinary button.
  { Rig r; Input& in = r.set("padhotkeys.modifier", "leftstick").go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(in.take_capture() == "guide"); }
  { Rig r; Input& in = r.set("padhotkeys.modifier", "leftstick").go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_LEFTSTICK, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_Y, true));
    CHECK(in.take_capture() == "mod+y"); }
  // The keyboard column has no modifier by default, so nothing waits there.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "J"); }
  { Rig r; Input& in = r.set("hotkeys.modifier", "left ctrl").go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_LCTRL, true));
    CHECK(in.take_capture().empty());
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "mod+J"); }
}

// The keyboard column cannot be satisfied from a controller, and a handheld
// has no keyboard at all -- so opening it there used to be a dead end, with
// the pad falling through to the menu underneath instead of getting out.
void test_keyboard_capture_backs_out_on_pad_input() {
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_A, true));   // swallowed...
    CHECK(!in.capturing());                                     // ... and backs out
    CHECK(in.take_capture().empty());                           // binding nothing
  }
  // A stick counts once it has actually been moved; its resting noise does not
  // cancel anything, but is still eaten rather than reaching the menu.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 900));
    CHECK(in.capturing());
    CHECK(T::capture_axis(in, SDL_CONTROLLER_AXIS_LEFTX, 30000));
    CHECK(!in.capturing());
    CHECK(in.take_capture().empty()); }
  // A key still binds, and Escape still cancels.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(T::capture_key(in, SDLK_j, true));
    CHECK(in.take_capture() == "J"); }
  // The window closing must still reach the frontend, capture or no capture.
  { Rig r; Input& in = r.go();
    in.begin_capture(false);
    CHECK(!T::capture_other(in, SDL_QUIT));
    CHECK(in.capturing()); }
}

// The warning is only useful if it means what it says, so it must account for
// every remaining way in: START confirms on every page but Controls, and the
// fallback layer holds any control nothing is bound to.
void test_reachable_counts_every_way_in() {
  { Rig r; Input& in = r.go();
    CHECK(T::reachable(in, B::BTN_A) && T::reachable(in, B::BTN_B));
    CHECK(in.collisions().empty()); }
  // A cleared, but START still opens rows.
  { Rig r; Input& in = r.set("keys.a", "none").go();
    CHECK(T::reachable(in, B::BTN_A)); }
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "F1").go();
    CHECK(T::reachable(in, B::BTN_A)); }
  // Both cleared frees Return, which the fallback then claims.
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "none").go();
    CHECK(T::key_free(in, SDLK_RETURN));
    CHECK(T::reachable(in, B::BTN_A)); }
  // Only with Return taken by something else is there nothing left.
  { Rig r; Input& in = r.set("keys.a", "none").set("keys.start", "none").set("keys.select", "Return").go();
    CHECK(!T::reachable(in, B::BTN_A));
    const std::vector<std::string> w = in.collisions();
    CHECK(!w.empty());
    // First: the page shows one line, and this is the one that cannot be
    // recovered from without a text editor.
    CHECK(w[0].find("UNREACHABLE") != std::string::npos); }
}

// The pen follows a stick, not one of its two axes, so the Controls page
// stores which stick the player pushed rather than the axis event it saw.
void test_stylus_axis_reads_the_stick() {
  for (const char* a : {"+leftx", "-leftx", "+lefty", "-lefty"})
    CHECK(std::string(Input::stylus_axis_of(a)) == "left");
  for (const char* a : {"+rightx", "-rightx", "+righty", "-righty"})
    CHECK(std::string(Input::stylus_axis_of(a)) == "right");
  // A trigger or a button says nothing about which stick, so the row keeps
  // what it had rather than guessing.
  for (const char* a : {"+lefttrigger", "+righttrigger", "leftstick", "a", "", "+"})
    CHECK(Input::stylus_axis_of(a) == nullptr);
}

// The page offers to reset these rows, so its idea of a default has to be the
// one configure() actually starts from.
void test_extra_defaults_match_configure() {
  CHECK(std::string(Input::mod_default(true)) == "guide");
  CHECK(std::string(Input::mod_default(false)) == "none");
  CHECK(std::string(Input::stylus_button_default()) == "rightstick");
  CHECK(std::string(Input::stylus_dpad_default()) == "none");
  { Config c; CHECK(std::string(Input::stylus_axis_default(c)) == "right"); }
  // "pad.stylus_stick = false" is the old spelling of "none" and still counts.
  { Config c; c.set("pad.stylus_stick", "false");
    CHECK(std::string(Input::stylus_axis_default(c)) == "none"); }
  // And a pad configured through those defaults binds what they say.
  { Rig r; Input& in = r.go();
    in.begin_capture(true);
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, true));
    CHECK(T::capture_pad(in, SDL_CONTROLLER_BUTTON_GUIDE, false));
    CHECK(in.take_capture() == Input::mod_default(true)); }
}

// Display only: the file keeps SDL's names, and every alias means exactly what
// the SDL spelling means.
void test_pad_labels() {
  CHECK(Input::pad_label("back") == "SELECT");
  CHECK(Input::pad_label("guide") == "FUNC BTN");
  CHECK(Input::pad_label("leftshoulder") == "L1");
  CHECK(Input::pad_label("+righttrigger") == "R2");
  CHECK(Input::pad_label("mod+start+back") == "MOD+START+SELECT");
  CHECK(Input::pad_label("b") == "\x02 EAST");      // the position pip and the position
  CHECK(Input::pad_label("+leftx") == "+LEFTX");    // no entry: upper case, as before
  const char* pairs[][2] = {{"south", "a"}, {"north", "y"}, {"east", "b"}, {"west", "x"},
                            {"select", "back"}, {"func", "guide"}, {"funcbtn", "guide"},
                            {"l1", "leftshoulder"}, {"r1", "rightshoulder"},
                            {"l2", "+lefttrigger"}, {"r2", "+righttrigger"},
                            {"l3", "leftstick"}, {"r3", "rightstick"},
                            {"mod+l1", "mod+leftshoulder"}, {"mod+start+select", "mod+start+back"}};
  for (const auto& p : pairs) CHECK(Input::pad_label(p[0]) == Input::pad_label(p[1]));
}

} // namespace
} // namespace ds::sdl

int main() {
  ds::sdl::test_fallback_is_inert_by_default();
  ds::sdl::test_fallback_rescues_an_orphaned_button();
  ds::sdl::test_fallback_yields_to_a_real_binding();
  ds::sdl::test_fallback_press_does_not_outlive_the_frame();
  ds::sdl::test_escape_is_still_quit();
  ds::sdl::test_escape_cancels_once_quit_moves();
  ds::sdl::test_pad_capture_swallows_keys_and_escape_cancels();
  ds::sdl::test_capture_composes_a_chord_with_the_modifier();
  ds::sdl::test_reachable_counts_every_way_in();
  ds::sdl::test_keyboard_capture_backs_out_on_pad_input();
  ds::sdl::test_stylus_axis_reads_the_stick();
  ds::sdl::test_extra_defaults_match_configure();
  ds::sdl::test_pad_labels();
  std::printf("input tests passed\n");
  return 0;
}
