// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// A destination for the frontend's own drawing, in DS pixel space (256x192).
// `xrun` maps a DS column onto the destination's columns (null: 1:1) and `h`
// is the destination height, so the same drawing code serves the scaled
// buffer the GPU produces and a plain copy of a framebuffer.
struct Blit { u32* px; u32 pitch; u32 h; const u16* xrun; };

// A 3x5 uppercase font, `scale` times. Lowercase is folded to uppercase and
// anything outside the table draws as a space. Returns the x past the string.
int  draw_text(const Blit& d, int x, int y, int scale, u32 colour, const char* s);
int  text_width(int scale, const char* s);
// Halves every pixel's brightness, in place, over a whole DS framebuffer.
void dim_framebuffer(u32* px, u32 n);

// The pause menu: a blitted, modal list drawn over the held last frame while
// emulation is stopped. It is not an overlay -- nothing runs behind it -- so
// it is drawn once per idle tick into a copy of the framebuffer and presented
// through the unscaled path.
//
// Navigation uses the player's own DS bindings (up/down to move, A to choose,
// B to go back), because those are the buttons a handheld has and they are
// already mapped; the guest cannot see them while the menu is up.
class Menu {
public:
  // What the frontend should do after input(). Save/Load act on slot().
  enum class Result : u8 { None, Resume, Save, Load, Quit };

  bool open() const { return open_; }
  void set_open(bool o) { open_ = o; page_ = Page::Root; row_ = 0; }

  int  slot() const { return slot_; }
  void set_slot(int s) { slot_ = s; }
  // Shown on the root row so the player can see what a save would overwrite.
  void set_slot_used(int s, bool used) { if (s >= 0 && s < 10) used_[s] = used; }

  Result input(u32 presses);       // a DS button mask (io::Io::Button bits)
  void   draw(const Blit& d) const;

  // The root page is a table in menu.cpp (kRoot); this is its length, and the
  // panel grows with it. Adding a row is one entry there -- eight rows still
  // fit the screen at this size, so the page has room to gain a few.
  static constexpr int kRootRows = 5;
  static constexpr int kSlotRows = 5;   // ten slots as two columns of five

private:
  enum class Page : u8 { Root, Slot };
  bool open_ = false;
  Page page_ = Page::Root;
  int  row_ = 0;
  int  slot_ = 0;
  bool used_[10] = {};
};

} // namespace ds::sdl
