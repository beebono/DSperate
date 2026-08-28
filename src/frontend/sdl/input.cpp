// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "display.h"
#include "core/nds.h"

#include <cstdio>

namespace ds::sdl {

using B = io::Io::Button;

namespace {

// Keyboard defaults follow the usual emulator layout (arrows + ZX/AS).
bool key_button(SDL_Keycode k, B& out) {
  switch (k) {
  case SDLK_x:      out = B::BTN_A; return true;
  case SDLK_z:      out = B::BTN_B; return true;
  case SDLK_s:      out = B::BTN_X; return true;
  case SDLK_a:      out = B::BTN_Y; return true;
  case SDLK_q:      out = B::BTN_L; return true;
  case SDLK_w:      out = B::BTN_R; return true;
  case SDLK_RETURN: out = B::BTN_START; return true;
  case SDLK_RSHIFT: case SDLK_BACKSPACE: out = B::BTN_SELECT; return true;
  case SDLK_UP:     out = B::BTN_UP; return true;
  case SDLK_DOWN:   out = B::BTN_DOWN; return true;
  case SDLK_LEFT:   out = B::BTN_LEFT; return true;
  case SDLK_RIGHT:  out = B::BTN_RIGHT; return true;
  default: return false;
  }
}

// SDL's controller buttons are named by position, so the DS's B (bottom) is
// SDL's A and the DS's A (right) is SDL's B.
bool pad_button(Uint8 b, B& out) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_A:             out = B::BTN_B; return true;
  case SDL_CONTROLLER_BUTTON_B:             out = B::BTN_A; return true;
  case SDL_CONTROLLER_BUTTON_X:             out = B::BTN_Y; return true;
  case SDL_CONTROLLER_BUTTON_Y:             out = B::BTN_X; return true;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  out = B::BTN_L; return true;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out = B::BTN_R; return true;
  case SDL_CONTROLLER_BUTTON_START:         out = B::BTN_START; return true;
  case SDL_CONTROLLER_BUTTON_BACK:          out = B::BTN_SELECT; return true;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:       out = B::BTN_UP; return true;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     out = B::BTN_DOWN; return true;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     out = B::BTN_LEFT; return true;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    out = B::BTN_RIGHT; return true;
  default: return false;
  }
}

} // namespace

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

void Input::touch_at(int wx, int wy, Display& display) {
  int screen = 0, sx = 0, sy = 0;
  if (!display.map_point(wx, wy, screen, sx, sy) || screen != 1) return;   // bottom screen only
  touching_ = true; touched_ = true; touch_x_ = sx; touch_y_ = sy;
}

void Input::handle(const SDL_Event& e, Display& display, Display* second) {
  // Route window-addressed events to the window they happened on; keyboard
  // and controller input is global.
  auto owner = [&](u32 wid) -> Display& {
    return (second && wid == second->window_id()) ? *second : display;
  };
  B b;
  switch (e.type) {
  case SDL_QUIT: quit_ = true; break;

  case SDL_KEYDOWN:
  case SDL_KEYUP: {
    if (e.key.repeat) break;
    const bool down = e.type == SDL_KEYDOWN;
    if (down && e.key.keysym.sym == SDLK_ESCAPE) { quit_ = true; break; }
    if (down && e.key.keysym.sym == SDLK_f) { display.toggle_fullscreen(); if (second) second->toggle_fullscreen(); break; }
    if (key_button(e.key.keysym.sym, b)) set(b, down);
    break;
  }

  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP:
    if (pad_button(e.cbutton.button, b)) set(b, e.type == SDL_CONTROLLERBUTTONDOWN);
    if (combo_quit()) quit_ = true;
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
