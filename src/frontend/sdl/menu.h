// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cheat/database.h"
#include "frontend/sdl/settings.h"

#include <string>
#include <vector>

namespace ds::sdl {

// A destination for drawing in DS pixel space (256x192): `xrun` maps a DS
// column onto the destination's columns (null: 1:1) and `h` is the
// destination height. This is what the stylus crosshair still uses -- it is a
// guest-space pointer and belongs on the bottom screen's view, wherever that
// view happens to be. Everything else draws in panel pixels; see Canvas.
struct Blit { u32* px; u32 pitch; u32 h; const u16* xrun; };
// DS pixel (x, y) -> the panel pixels it covers in `d`. Columns go by the
// view's width (xrun's last entry), the way rows go by its height, not
// through the run table itself: under chunky that table is the cell map
// (pairs merged, or one run per panel cell with the rest empty), and an
// overlay routed through it lost every other column, or landed on the
// empty runs past the last cell and vanished -- the FPS counter, anchored
// at the right edge, did exactly that whenever a panel cell was in use.
struct BlitRect { u32 x0, x1, y0, y1; };
inline BlitRect blit_rect(const Blit& d, int x, int y) {
  const u32 w = d.xrun ? d.xrun[ds::SCREEN_W] : ds::SCREEN_W;
  return BlitRect{w * static_cast<u32>(x) / ds::SCREEN_W, w * static_cast<u32>(x + 1) / ds::SCREEN_W,
                  d.h * static_cast<u32>(y) / ds::SCREEN_H, d.h * static_cast<u32>(y + 1) / ds::SCREEN_H};
}

// The frontend's own drawing surface, in the output's own pixels: the whole
// window or panel, not one DS screen's slice of it. Coordinates are plain
// pixels -- no run table, no DS grid -- which is what lets a page be laid out
// for the screen it is actually on rather than for a 256x192 framebuffer that
// is then upscaled with the picture.
//
// Two tiers have no panel-resolution buffer the CPU may write (the display
// engine's scaler reads DS-sized buffers; the SDL_Renderer path has no frame
// buffer at all). There, the same drawing code is handed a Canvas over a
// 256x192 scratch that goes on to be scaled like a frame: one drawing path,
// two sources. See Display::canvas_capable().
struct Canvas { u32* px; u32 pitch; int w, h; };

// What a piece of drawing covered, so the frontend can tell the display which
// part of the canvas it touched (Display::note_canvas_draw). Empty when
// nothing was drawn.
struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// Face-button position pips: a diamond of four with the named one filled.
// They are control bytes so they can sit inside an ordinary string -- "\x01 A"
// draws the pip for the bottom button and then its SDL letter.
constexpr char kFaceSouth = '\x01';   // bottom  (SDL a)
constexpr char kFaceEast  = '\x02';   // right   (SDL b)
constexpr char kFaceWest  = '\x03';   // left    (SDL x)
constexpr char kFaceNorth = '\x04';   // top     (SDL y)

// A 5x7 uppercase font, `scale` times. Lowercase is folded to uppercase and
// anything outside the table draws as a space. Returns the x past the string.
// `keep_case` draws a-z with the lower-case glyphs instead of folding them
// onto the capitals. Off everywhere but the name editor, where the player has
// to be able to see which case they are typing.
int  draw_text(const Canvas& d, int x, int y, int scale, u32 colour, const char* s, bool keep_case = false);
int  text_width(int scale, const char* s);
// The glyph scale a canvas of this size should draw at. The old menu drew at
// scale 2 into a 256-wide buffer that was then upscaled to the panel, so twice
// the panel's reduction is the same apparent size on the glass: a page keeps
// the size it had and gains crispness, rather than shrinking to gain rows.
int  ui_scale(const Canvas& d);
// Halves every pixel's brightness, in place, over a whole framebuffer.
// What the achievement pages read and act on. An interface for the same reason
// SettingsHost is one: menu.cpp must not depend on the RetroAchievements
// library, so the pause menu still builds -- and still has tests -- when
// DSPERATE_CHEEVOS is off. main.cpp implements it over ds::cheevos::Client.
//
// Everything is a formatted string rather than the library's own types: the
// menu's job is to draw lines, not to know what an achievement is.
struct CheevosHost {
  virtual ~CheevosHost() = default;

  struct Row {
    std::string title;       // the achievement's name
    std::string detail;      // its description, or measured progress
    u32  points = 0;
    bool unlocked = false;
    bool unsupported = false;   // a condition reads memory DSperate does not map
  };

  // One line for the account page: signed in as whom, signed out, or why there
  // is nothing to show (no network on this device, no set for this ROM).
  virtual std::string status() const = 0;
  // "2/110 EARNED  1/1090 POINTS", or empty when no set is loaded.
  virtual std::string progress() const = 0;
  virtual bool signed_in() const = 0;
  virtual bool has_set() const = 0;

  virtual int  row_count() const = 0;
  virtual Row  row(int i) const = 0;

  // Both asynchronous: the menu shows whatever status() says on a later frame.
  virtual void sign_in(const std::string& username, const std::string& password) = 0;
  virtual void sign_out() = 0;

  // The switches the account page offers. An enum rather than config keys so
  // menu.cpp does not have to know what they are called; the frontend maps
  // them onto cheevos.* and persists them the way every other setting is
  // persisted.
  enum class Option : u8 { Toasts, Screenshot, Encore };
  virtual bool option(Option) const = 0;
  virtual void set_option(Option, bool on) = 0;
};

void dim_framebuffer(u32* px, u32 n);
// A small centred panel with a title, a second line at the same size, and a
// dim third one: the "unpacking" notice a first launch of a zipped game
// shows while the image is written. Drawn over a dimmed frame like the menu.
void draw_notice(const Canvas& d, const char* title, const char* line2, const char* line3);

// An achievement unlock, in the bottom-right corner. A small panel rather than
// draw_notice's centred one, because this appears while the game is being
// played rather than instead of it, and in the corner because the DS picture
// is centred. It sizes itself to its text up to two thirds of the canvas and
// truncates beyond that. `detail` may be empty, and `points` is drawn only
// when non-zero -- RetroAchievements has 0-point achievements and "0 PTS"
// reads like a bug.
// `header` is the small accent line above the title ("ACHIEVEMENT UNLOCKED"),
// which is what makes an unlock read as one rather than as a stray message; it
// may be null. `detail` may be null too, and `points` is drawn only when
// non-zero.
void draw_toast(const Canvas& d, const char* header, const char* title, const char* detail, u32 points);
// Where draw_toast will put it, so the caller can tell Display which part of
// the canvas changed rather than repainting all of it.
Rect toast_rect(const Canvas& d, const char* header, const char* title, const char* detail, u32 points);

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

  // The options pages. Without a host there is no OPTIONS row: the menu has
  // nothing to read a setting from and nowhere to put one.
  void set_settings_host(SettingsHost* host) { host_ = host; }
  // Null when the build has no RetroAchievements support or it is switched
  // off; the root row then does not appear at all.
  void set_cheevos_host(CheevosHost* host) { cheevos_ = host; }
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
  void   draw(const Canvas& d) const;

  // True when the picture would differ from the last draw: a press was acted
  // on, a repeat fired, or a name is part way through scrolling. The frontend
  // composites only when this says to, since nothing else is running.
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // The root page is a table in menu.cpp (kRoot); this is its length, and the
  // panel grows with it. Adding a row is one entry there: the panel is sized
  // at draw time from the canvas, so a page no longer has a fixed ceiling.
  static constexpr int kRootRows = 8;
  static constexpr int kSlotRows = 5;   // ten slots as two columns of five

private:
  // A page stack rather than a flat state, so B pops wherever it is pressed
  // and a page three deep needs no special case. Games is the exception it
  // always was: it is raised as the launcher's own modal page with nothing
  // behind it, so it is pushed onto an empty stack and B does not leave it.
  enum class Page : u8 { Root, Slot, Cheats, Games, Options, Emulation, VisualFx, Layout, Controls, DsOptions, TextEdit, Cheevos, CheevosAccount };
  static constexpr int kMaxDepth = 6;
  Page stack_[kMaxDepth] = {Page::Root};
  int  depth_ = 1;
  Page page() const { return stack_[depth_ - 1]; }
  void push(Page p);
  bool pop();                    // false at the root, where B resumes instead
  // One line of the cheats page. Headings and notes are shown but cannot be
  // selected; the list is built once when the page opens.
  struct Line { enum Kind : u8 { Heading, Note, Toggle } kind; int at; };
  bool open_ = false;
  int  row_ = 0;        // the root page's selection
  int  slot_row_ = 0;   // the slot page's, kept apart so backing out lands where it left
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
  // How many rows the scrolling pages last fitted on screen. update() needs it
  // -- the shoulders page by it, and the scroll clamps use it -- but only draw
  // knows the canvas, so it is measured there, exactly as the marquee is. The
  // initial value only matters if a key is pressed before the first draw.
  mutable int visible_ = 12;
  bool have_cheats() const { return codes_ && !codes_->empty(); }
  // The root page hides the cheats row when there is nothing to show, so the
  // rows on screen are not the table's rows; this maps one to the other.
  bool root_visible(int item) const;
  int root_rows() const;
  int root_item(int row) const;
  SettingsHost* host_ = nullptr;
  // One row per page, kept while the menu is open so backing out of a page
  // and into it again lands where it was left.
  int  opt_row_ = 0;
  int  set_row_[4] = {};         // Emulation, VisualFx, Layout, DsOptions
  // Written by draw: how far a page is scrolled depends on how many rows the
  // canvas fits, which only draw knows -- the same reason visible_ is mutable.
  mutable int set_top_[4] = {};
  bool have_options() const { return host_ != nullptr; }
  // The table a settings page shows, and where its row state lives.
  const Setting* table() const;
  int  table_slot() const;
  int  settings_rows() const;
  // Skips rows the host has switched off, the way the cheats list skips its
  // headings. Returns false when there is nothing selectable in that
  // direction, which leaves the selection where it was.
  bool move_setting_row(int delta);
  void step_setting(int dir);
  void draw_cheats(const Canvas& d) const;
  void draw_cheevos(const Canvas& d) const;
  void draw_cheevos_account(const Canvas& d) const;
  Result handle_cheevos(u32 presses);
  Result handle_cheevos_account(u32 presses);
  void move_cheevos_row(int delta);
  bool have_cheevos() const { return cheevos_ != nullptr; }
  CheevosHost* cheevos_ = nullptr;
  int cheevos_row_ = 0, cheevos_top_ = 0;
  int account_row_ = 0;
  void draw_games(const Canvas& d) const;
  void draw_options(const Canvas& d) const;
  void draw_settings(const Canvas& d) const;
  void draw_controls(const Canvas& d) const;
  Result handle_options(u32 presses);
  Result handle_settings(u32 presses);
  Result handle_controls(u32 presses);
  void draw_text_edit(const Canvas& d) const;
  bool listen_shown_ = false;   // the Controls page last drew the listening prompt
  Result handle_text_edit(u32 presses);
  // The character editor, for the two free-text [user] fields. A handheld has
  // no keyboard, so a character is chosen by cycling rather than typed: up
  // and down walk the current table, the shoulders change table, left and
  // right move along the field.
  void open_text_edit();
  // The label is kept with the buffer, not looked up when drawing: by then
  // TextEdit is the page on top and table() no longer names the row that
  // opened it.
  // Where an accepted edit goes. The editor is identical either way -- same
  // character tables, same keys -- so rather than a second page it grew a
  // destination. CheevosPassword also hides what it has collected.
  enum class EditDest : u8 { Setting, CheevosUser, CheevosPassword };
  std::string edit_key_, edit_label_, edit_buf_;
  int  edit_pos_ = 0, edit_table_ = 0, edit_max_ = 0;
  EditDest edit_dest_ = EditDest::Setting;
  void open_credential_edit(EditDest dest);
  std::string pending_user_;   // held between the two prompts; the password
                               // goes straight to sign_in and is never stored
  // The Controls page: which column (keyboard or pad), where in it, and how
  // far it is scrolled. `bind_row_` is an index into the host's binding list.
  bool bind_pad_ = false;
  int  bind_row_ = 0;
  mutable int bind_top_ = 0;
  void move_bind_row(int delta);
  void build_lines();
  void move_cheat_row(int delta);
  void move_game_row(int delta);
  // The two scrolling pages share the repeat and marquee timing, which both
  // key off "has the selection moved"; this is the selection they mean.
  int  list_row() const;
  bool list_page() const { return page() == Page::Cheats || page() == Page::Games || settings_page() || controls_page(); }
  bool settings_page() const { return page() == Page::Emulation || page() == Page::VisualFx || page() == Page::Layout || page() == Page::DsOptions; }
  bool controls_page() const { return page() == Page::Controls; }
  void toggle_cheat();
};

} // namespace ds::sdl
