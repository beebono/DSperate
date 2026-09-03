// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cheat/database.h"

#include <string>
#include <vector>

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
// A small centred panel with a title, a second line at the same size, and a
// dim third one: the "unpacking" notice a first launch of a zipped game
// shows while the image is written. Drawn over a dimmed frame like the menu.
void draw_notice(const Blit& d, const char* title, const char* line2, const char* line3);

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
  // What the frontend should do after input(). Save/Load act on slot();
  // Launch acts on chosen().
  enum class Result : u8 { None, Resume, Save, Load, Quit, Launch };

  // One game in the picker. `title` is what the list shows -- the ROM
  // header's own title where it could be read, the filename otherwise -- and
  // `path` is what gets loaded.
  struct GameEntry { std::string title, path; };

  bool open() const { return open_; }
  void set_open(bool o);

  // Cheats. The menu edits `codes` in place -- it is the engine's own vector,
  // so a toggle takes effect on the next frame with no copying back -- and
  // reads `groups` for the headings and for which groups are a set of
  // alternatives. Passing null, or an empty list, hides the row entirely:
  // there is no point offering a page that cannot have anything on it.
  void set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups);
  // Set when a toggle changed something, so the frontend knows to save the
  // selection. Cleared by the frontend once it has.
  bool cheats_dirty() const { return cheats_dirty_; }
  void clear_cheats_dirty() { cheats_dirty_ = false; }

  // The game picker. The list is owned by the frontend (it is built once at
  // boot) and the menu only points at it. Passing null, or an empty list,
  // leaves open_games() with nothing to show, which it says on the page
  // rather than by refusing to open: "no games" is the answer to the
  // player's question, and a blank screen is not.
  void set_games(const std::vector<GameEntry>* games) { games_ = games; }
  // Raise the picker as the launcher's own modal page: not reached from the
  // root menu, and B does not back out of it, because there is nothing
  // behind it to go back to -- the loader cart has faded to white and is
  // spinning. The way out is to choose a game (or to quit the emulator,
  // which the window close and the quit hotkey still do).
  void open_games();
  // The path of the game picked, valid when update() returned Launch.
  const std::string& chosen() const { return chosen_; }

  int  slot() const { return slot_; }
  void set_slot(int s) { slot_ = s; }
  // Shown on the root row so the player can see what a save would overwrite.
  void set_slot_used(int s, bool used) { if (s >= 0 && s < 10) used_[s] = used; }

  // One menu tick. `presses` are the button edges since the last call, `held`
  // is what is down now (for key repeat) and `ms` is how long since the last
  // call (for repeat and for scrolling a name too long to fit). A menu that
  // is only ever given edges still works; it simply does neither.
  Result update(u32 presses, u32 held, u32 ms);
  Result input(u32 presses) { return update(presses, 0, 0); }
  void   draw(const Blit& d) const;

  // True when the picture would differ from the last draw: a press was acted
  // on, a repeat fired, or a name is part way through scrolling. The frontend
  // composites only when this says to, since nothing else is running.
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // The root page is a table in menu.cpp (kRoot); this is its length, and the
  // panel grows with it. Adding a row is one entry there -- eight rows still
  // fit the screen at this size, so the page has room to gain a few.
  static constexpr int kRootRows = 6;
  static constexpr int kSlotRows = 5;   // ten slots as two columns of five

private:
  enum class Page : u8 { Root, Slot, Cheats, Games };
  // One line of the cheats page. Headings and notes are shown but cannot be
  // selected; the list is built once when the page opens.
  struct Line { enum Kind : u8 { Heading, Note, Toggle } kind; int at; };
  bool open_ = false;
  Page page_ = Page::Root;
  int  row_ = 0;
  int  slot_ = 0;
  bool used_[10] = {};

  std::vector<cheat::Code>* codes_ = nullptr;
  const std::vector<cheat::Group>* groups_ = nullptr;
  std::vector<Line> lines_;      // the cheats page, headings and all
  int  cheat_row_ = 0;           // index into lines_
  int  cheat_top_ = 0;           // first line shown, for scrolling
  bool cheats_dirty_ = false;

  const std::vector<GameEntry>* games_ = nullptr;
  int  game_row_ = 0;            // index into *games_
  int  game_top_ = 0;            // first game shown, for scrolling
  std::string chosen_;           // the path Result::Launch names

  // Holding a direction walks the list: 400 ms before it starts, then one
  // row every 55 ms, which is brisk enough for a list of thousands without
  // running away on a list of six.
  static constexpr u32 kRepeatDelayMs = 400, kRepeatRateMs = 55;
  // A selected name too long for the panel scrolls sideways so the rest can
  // be read: still for half a second, then 26 px a second, then a pause at
  // the end before it snaps back and does it again.
  static constexpr u32 kMarqueeDelayMs = 500, kMarqueeHoldMs = 900, kMarqueePxPerSec = 26;

  bool dirty_ = false;
  int  repeat_dir_ = 0;          // -1 up, +1 down, 0 nothing held
  u32  repeat_ms_ = 0;
  bool repeating_ = false;       // past the initial delay
  u32  marquee_ms_ = 0;          // since the selection last moved

  // How far the selected name is scrolled, given how much of it overflows.
  Result handle(u32 presses);    // the button handling, without the timing
  int marquee_offset(int overflow) const;
  // How far the selected name overruns its row, measured by the last draw so
  // update() can animate it without re-measuring the layout.
  mutable int marquee_overflow_ = 0;
  bool have_cheats() const { return codes_ && !codes_->empty(); }
  // The root page hides the cheats row when there is nothing to show, so the
  // rows on screen are not the table's rows; this maps one to the other.
  int root_rows() const;
  int root_item(int row) const;
  void draw_cheats(const Blit& d) const;
  void draw_games(const Blit& d) const;
  void build_lines();
  void move_cheat_row(int delta);
  void move_game_row(int delta);
  // The two scrolling pages share the repeat and marquee timing, which both
  // key off "has the selection moved"; this is the selection they mean.
  int  list_row() const;
  bool list_page() const { return page_ == Page::Cheats || page_ == Page::Games; }
  void toggle_cheat();
};

} // namespace ds::sdl
