// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"

#include <cstdio>
#include <string>
#include <iterator>

namespace ds::sdl {
namespace {

// 5x7 glyphs, one byte per row, bits 4..0 left to right; ' ' (0x20) to ']'
// (0x5D) -- the brackets are there because the cheats page draws its
// checkboxes with them.
// Wider than the 3x5 the slot digit uses: at this size M/N/W are distinct and
// a word is read rather than decoded, which a menu needs and an OSD digit
// does not.
constexpr u8 kFont[62][7] = {
  { 0, 0, 0, 0, 0, 0, 0}, { 4, 4, 4, 4, 4, 0, 4}, {10,10, 0, 0, 0, 0, 0}, {10,10,31,10,31,10,10},
  { 4,15,20,14, 5,30, 4}, {24,25, 2, 4, 8,19, 3}, { 8,20,20, 8,21,18,13}, { 4, 4, 0, 0, 0, 0, 0},
  { 2, 4, 8, 8, 8, 4, 2}, { 8, 4, 2, 2, 2, 4, 8}, { 0, 4,21,14,21, 4, 0}, { 0, 4, 4,31, 4, 4, 0},
  { 0, 0, 0, 0, 4, 4, 8}, { 0, 0, 0,31, 0, 0, 0}, { 0, 0, 0, 0, 0,12,12}, { 1, 1, 2, 4, 8,16,16},
  {14,17,19,21,25,17,14}, { 4,12, 4, 4, 4, 4,14}, {14,17, 1, 2, 4, 8,31}, {31, 2, 4, 2, 1,17,14},
  { 2, 6,10,18,31, 2, 2}, {31,16,30, 1, 1,17,14}, { 6, 8,16,30,17,17,14}, {31, 1, 2, 4, 8, 8, 8},
  {14,17,17,14,17,17,14}, {14,17,17,15, 1, 2,12},
  { 0,12,12, 0,12,12, 0}, { 0,12,12, 0,12, 4, 8}, { 2, 4, 8,16, 8, 4, 2}, { 0, 0,31, 0,31, 0, 0},
  { 8, 4, 2, 1, 2, 4, 8}, {14,17, 1, 2, 4, 0, 4}, {14,17,23,21,23,16,14},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14, 4, 4, 4, 4, 4,14}, { 7, 2, 2, 2, 2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,17,25,21,19,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14, 1, 1,30}, {31, 4, 4, 4, 4, 4, 4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10, 4}, {17,17,17,21,21,27,17}, {17,17,10, 4,10,17,17},
  {17,17,10, 4, 4, 4, 4}, {31, 1, 2, 4, 8,16,31},
  { 6, 4, 4, 4, 4, 4, 6}, {16,16, 8, 4, 2, 1, 1}, {12, 4, 4, 4, 4, 4,12}};

constexpr int kGlyphW = 5, kGlyphH = 7, kAdvance = 6;   // advance includes the one-pixel gap

// Panel geometry. Rows are `kRowH` tall around a `kGlyphH * kScale` glyph, so
// a selected row's bar sits evenly above and below its text -- derive it, do
// not hand-tune it, or the bar drifts off the text the next time either
// changes. The header keeps its own scale; rows share kScale.
constexpr int kScale = 2, kGlyphPx = kGlyphH * kScale, kRowH = kGlyphPx + 4;
constexpr int kTitleY = 8, kRuleY = kTitleY + kGlyphPx + 6, kRowsY = kRuleY + 8;
constexpr int panel_height(int rows) { return kRowsY + rows * kRowH + 8; }

// Cheat names are sentences ("Press L+R+SELECT For 7 Red Coins") and a game
// can have thousands of them, so that page drops to single-scale rows: about
// thirty characters across and twelve of them at a time, against fifteen and
// five. The heading keeps the larger size.
constexpr int kCheatScale = 1, kCheatGlyphPx = kGlyphH * kCheatScale, kCheatRowH = kCheatGlyphPx + 4;
constexpr int kCheatPanelW = 240, kCheatPanelH = 178;
constexpr int kCheatRowsY = kRuleY + 6;
constexpr int kCheatVisible = (kCheatPanelH - kCheatRowsY - 8) / kCheatRowH;
static_assert(kCheatVisible >= 8, "the cheats page should show a useful number of rows");

int glyph(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  const int i = static_cast<int>(static_cast<unsigned char>(c)) - 0x20;
  return (i >= 0 && i < 62) ? i : 0;
}

// One DS pixel, mapped onto the destination. Clipped to the DS screen so a
// caller may lay out past the edges without checking.
// Columns outside [clip_x0, clip_x1) are dropped, which is what lets a name
// scroll under the panel edge instead of over it. The default admits the
// whole screen.
int g_clip_x0 = 0, g_clip_x1 = static_cast<int>(ds::SCREEN_W);

void put(const Blit& d, int x, int y, u32 colour) {
  if (x < g_clip_x0 || x >= g_clip_x1) return;
  if (x < 0 || x >= static_cast<int>(ds::SCREEN_W) || y < 0 || y >= static_cast<int>(ds::SCREEN_H)) return;
  const u32 x0 = d.xrun ? d.xrun[x] : static_cast<u32>(x), x1 = d.xrun ? d.xrun[x + 1] : static_cast<u32>(x + 1);
  const u32 y0 = d.h * static_cast<u32>(y) / ds::SCREEN_H, y1 = d.h * static_cast<u32>(y + 1) / ds::SCREEN_H;
  for (u32 yy = y0; yy < y1; ++yy)
    for (u32 xx = x0; xx < x1; ++xx) d.px[yy * d.pitch + xx] = colour;
}

void fill_rect(const Blit& d, int x, int y, int w, int h, u32 colour) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) put(d, xx, yy, colour);
}

constexpr u32 kInk = 0xFFFFFFFF, kDim = 0xFF909090, kPanel = 0xFF101018, kEdge = 0xFF5060A0, kSel = 0xFF3050A0;
constexpr u32 kEdgeText = 0xFFA0B0E0, kPanelEdgeDim = 0xFF303040;   // group headings; the scroll-bar track

// The panel every page sits in: a filled box with a one-pixel edge.
void panel(const Blit& d, int x, int y, int w, int h) {
  fill_rect(d, x, y, w, h, kPanel);
  fill_rect(d, x, y, w, 1, kEdge);
  fill_rect(d, x, y + h - 1, w, 1, kEdge);
  fill_rect(d, x, y, 1, h, kEdge);
  fill_rect(d, x + w - 1, y, 1, h, kEdge);
}

// A scroll bar for a list page, because the list gives no other clue how long
// it is: some games have five thousand cheats, and a library can be as long.
// Drawn only when there is something off screen.
void scroll_bar(const Blit& d, int px0, int py0, int n, int top) {
  if (n <= kCheatVisible) return;
  const int track_x = px0 + kCheatPanelW - 5, track_y = py0 + kCheatRowsY - 2;
  const int track_h = kCheatVisible * kCheatRowH;
  fill_rect(d, track_x, track_y, 2, track_h, kPanelEdgeDim);
  int bar = track_h * kCheatVisible / n;
  if (bar < 4) bar = 4;
  const int span = track_h - bar;
  fill_rect(d, track_x, track_y + (span > 0 ? span * top / (n - kCheatVisible) : 0), 2, bar, kEdge);
}

// The root page, in order. A null label is the slot row: it is formatted from
// the current slot and opens the slot page rather than returning a result.
// New entries go here and nowhere else -- the panel sizes itself to the count.
constexpr int kSlotRow = 2;
constexpr int kCheatRow = 3;   // hidden when no database matched this ROM
constexpr struct RootItem { const char* label; Menu::Result result; } kRoot[] = {
  {"SAVE STATE", Menu::Result::Save},
  {"LOAD STATE", Menu::Result::Load},
  {nullptr,      Menu::Result::None},
  {"CHEATS",     Menu::Result::None},
  {"RESUME",     Menu::Result::Resume},
  {"QUIT",       Menu::Result::Quit},
};
static_assert(static_cast<int>(std::size(kRoot)) == Menu::kRootRows, "kRootRows must match the table");
static_assert(kRoot[kSlotRow].label == nullptr, "kSlotRow must name the slot row");
static_assert(kRoot[kCheatRow].result == Menu::Result::None, "kCheatRow must name the cheats row");
// Rows are cheap to add (one entry above) right up until the panel leaves the
// screen; at this size the root page holds eight. Past that, shrink the row
// text or paginate -- do not let it clip, which `put` would do in silence.
static_assert(panel_height(Menu::kRootRows) <= static_cast<int>(ds::SCREEN_H), "the root page no longer fits the screen");
static_assert(panel_height(Menu::kSlotRows) <= static_cast<int>(ds::SCREEN_H), "the slot page no longer fits the screen");

} // namespace

int text_width(int scale, const char* s) {
  int n = 0;
  for (const char* p = s; *p; ++p) ++n;
  return n > 0 ? n * kAdvance * scale - scale : 0;   // no gap after the last glyph
}

int draw_text(const Blit& d, int x, int y, int scale, u32 colour, const char* s) {
  for (const char* p = s; *p; ++p) {
    const u8* g = kFont[glyph(*p)];
    for (int r = 0; r < kGlyphH; ++r)
      for (int c = 0; c < kGlyphW; ++c)
        if ((g[r] >> (kGlyphW - 1 - c)) & 1)
          fill_rect(d, x + c * scale, y + r * scale, scale, scale, colour);
    x += kAdvance * scale;
  }
  return x - scale;
}

void dim_framebuffer(u32* px, u32 n) {
  for (u32 i = 0; i < n; ++i) px[i] = 0xFF000000 | ((px[i] >> 1) & 0x007F7F7F);
}



void Menu::set_open(bool o) {
  open_ = o;
  page_ = Page::Root;
  row_ = 0;
}

void Menu::set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) {
  codes_ = codes;
  groups_ = groups;
  lines_.clear();
}

// The root page hides the cheats row when nothing matched, so what is on
// screen is a subset of the table.
int Menu::root_rows() const { return have_cheats() ? kRootRows : kRootRows - 1; }

int Menu::root_item(int row) const {
  return (!have_cheats() && row >= kCheatRow) ? row + 1 : row;
}

// The display list: a heading wherever the group changes, then every code.
// Notes keep their place in it, because where the database's author put a
// "(M) must be on" is what tells you which codes it applies to.
void Menu::build_lines() {
  lines_.clear();
  if (!codes_) return;
  int last = -2;
  for (size_t i = 0; i < codes_->size(); ++i) {
    const cheat::Code& c = (*codes_)[i];
    if (c.group != last) {
      if (c.group >= 0 && groups_ && static_cast<size_t>(c.group) < groups_->size())
        lines_.push_back({Line::Heading, c.group});
      last = c.group;
    }
    lines_.push_back({c.is_note() ? Line::Note : Line::Toggle, static_cast<int>(i)});
  }
}

// Moves to the next selectable line in `delta`'s direction, stopping at the
// ends rather than wrapping: a list of thousands is not one to wrap around by
// accident, and the ends are where the scroll bar says you are.
void Menu::move_cheat_row(int delta) {
  if (lines_.empty()) return;
  const int n = static_cast<int>(lines_.size());
  int at = cheat_row_;
  for (int step = 0; step < n; ++step) {
    at += delta > 0 ? 1 : -1;
    if (at < 0 || at >= n) return;
    if (lines_[static_cast<size_t>(at)].kind == Line::Toggle) {
      cheat_row_ = at;
      // Scroll only as far as it takes to bring the selection back into view,
      // so paging through a long list does not reset it to the top edge.
      if (cheat_row_ < cheat_top_) cheat_top_ = cheat_row_;
      if (cheat_row_ >= cheat_top_ + kCheatVisible) cheat_top_ = cheat_row_ - kCheatVisible + 1;
      if (cheat_top_ > n - kCheatVisible) cheat_top_ = n - kCheatVisible;
      if (cheat_top_ < 0) cheat_top_ = 0;
      return;
    }
  }
}

void Menu::toggle_cheat() {
  if (!codes_ || lines_.empty()) return;
  const Line& l = lines_[static_cast<size_t>(cheat_row_)];
  if (l.kind != Line::Toggle) return;
  cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
  c.enabled = !c.enabled;
  cheats_dirty_ = true;
  // In a group the database marked as alternatives, turning one on turns the
  // rest off -- they are a choice of difficulty or character, not switches,
  // and two at once is what the flag exists to prevent.
  if (!c.enabled || c.group < 0 || !groups_ || static_cast<size_t>(c.group) >= groups_->size()) return;
  if (!(*groups_)[static_cast<size_t>(c.group)].exclusive) return;
  for (size_t i = 0; i < codes_->size(); ++i) {
    cheat::Code& other = (*codes_)[i];
    if (i != static_cast<size_t>(l.at) && other.group == c.group) other.enabled = false;
  }
}

void Menu::open_games() {
  open_ = true;
  page_ = Page::Games;
  game_row_ = 0;
  game_top_ = 0;
  chosen_.clear();
  marquee_ms_ = 0;
  dirty_ = true;
}

// The games list is selectable all the way down -- there are no headings in
// it -- so this is the plain clamped step, with the same "scroll only as far
// as it takes" rule the cheats list uses.
void Menu::move_game_row(int delta) {
  if (!games_ || games_->empty()) return;
  const int n = static_cast<int>(games_->size());
  const int at = game_row_ + delta;
  game_row_ = at < 0 ? 0 : at >= n ? n - 1 : at;
  if (game_row_ < game_top_) game_top_ = game_row_;
  if (game_row_ >= game_top_ + kCheatVisible) game_top_ = game_row_ - kCheatVisible + 1;
  if (game_top_ > n - kCheatVisible) game_top_ = n - kCheatVisible;
  if (game_top_ < 0) game_top_ = 0;
}

int Menu::list_row() const { return page_ == Page::Games ? game_row_ : cheat_row_; }

int Menu::marquee_offset(int overflow) const {
  if (overflow <= 0) return 0;
  const u32 scroll_ms = static_cast<u32>(overflow) * 1000u / kMarqueePxPerSec;
  const u32 cycle = kMarqueeDelayMs + scroll_ms + kMarqueeHoldMs;
  const u32 t = marquee_ms_ % cycle;    // still, scroll, hold, and round again
  if (t < kMarqueeDelayMs) return 0;
  if (t < kMarqueeDelayMs + scroll_ms)
    return static_cast<int>((t - kMarqueeDelayMs) * kMarqueePxPerSec / 1000u);
  return overflow;
}

Menu::Result Menu::update(u32 presses, u32 held, u32 ms) {
  using B = io::Io::Button;
  const int was_row = list_row();
  const int before = marquee_offset(marquee_overflow_);
  if (presses) dirty_ = true;

  // Key repeat, on the cheats page only: the other pages are a handful of
  // rows where a held direction would overshoot more often than it helps.
  if (list_page()) {
    const int dir = (held & (1u << B::BTN_UP)) ? -1 : (held & (1u << B::BTN_DOWN)) ? 1 : 0;
    if (dir != repeat_dir_) { repeat_dir_ = dir; repeat_ms_ = 0; repeating_ = false; }
    else if (dir != 0) {
      repeat_ms_ += ms;
      for (u32 step = repeating_ ? kRepeatRateMs : kRepeatDelayMs;
           repeat_ms_ >= step; step = kRepeatRateMs) {
        repeat_ms_ -= step;
        repeating_ = true;
        if (page_ == Page::Games) move_game_row(dir); else move_cheat_row(dir);
        dirty_ = true;
      }
    }
  } else {
    repeat_dir_ = 0; repeat_ms_ = 0; repeating_ = false;
  }

  const Result r = handle(presses);

  // A name only scrolls once the selection has settled on it.
  if (list_row() != was_row) marquee_ms_ = 0;
  else marquee_ms_ += ms;
  if (marquee_offset(marquee_overflow_) != before) dirty_ = true;
  return r;
}

Menu::Result Menu::handle(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (page_ == Page::Slot) {
    // Ten slots as two columns of five: up/down walk a column, left/right
    // cross between them. row_ is the slot itself, 0-4 left and 5-9 right.
    if (hit(B::BTN_UP))    row_ = (row_ % kSlotRows == 0) ? row_ + kSlotRows - 1 : row_ - 1;
    if (hit(B::BTN_DOWN))  row_ = (row_ % kSlotRows == kSlotRows - 1) ? row_ - kSlotRows + 1 : row_ + 1;
    if (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT)) row_ = (row_ + kSlotRows) % 10;
    if (hit(B::BTN_B)) { page_ = Page::Root; row_ = kSlotRow; return Result::None; }
    if (hit(B::BTN_A) || hit(B::BTN_START)) { slot_ = row_; page_ = Page::Root; row_ = kSlotRow; }
    return Result::None;
  }
  if (page_ == Page::Games) {
    // The same walk as the cheats page: up/down step, the shoulders page.
    // A is the only way off it -- see open_games() on why B is inert.
    if (hit(B::BTN_UP))   move_game_row(-1);
    if (hit(B::BTN_DOWN)) move_game_row(+1);
    if (hit(B::BTN_L)) move_game_row(-kCheatVisible);
    if (hit(B::BTN_R)) move_game_row(+kCheatVisible);
    if ((hit(B::BTN_A) || hit(B::BTN_START)) && games_ && !games_->empty()) {
      chosen_ = (*games_)[static_cast<size_t>(game_row_)].path;
      return Result::Launch;
    }
    return Result::None;
  }
  if (page_ == Page::Cheats) {
    // Up/down step, the shoulders page: a list of thousands is not one to
    // walk a row at a time.
    if (hit(B::BTN_UP))   move_cheat_row(-1);
    if (hit(B::BTN_DOWN)) move_cheat_row(+1);
    if (hit(B::BTN_L)) for (int i = 0; i < kCheatVisible; ++i) move_cheat_row(-1);
    if (hit(B::BTN_R)) for (int i = 0; i < kCheatVisible; ++i) move_cheat_row(+1);
    if (hit(B::BTN_A)) toggle_cheat();
    if (hit(B::BTN_B)) { page_ = Page::Root; row_ = kCheatRow; }
    return Result::None;
  }
  const int rows = root_rows();
  if (hit(B::BTN_UP))   row_ = (row_ + rows - 1) % rows;
  if (hit(B::BTN_DOWN)) row_ = (row_ + 1) % rows;
  // Left/right are a shortcut on the slot row, so the common adjustment does
  // not need the page at all.
  const int item = root_item(row_);
  if (item == kSlotRow) {
    if (hit(B::BTN_LEFT))  slot_ = (slot_ + 9) % 10;
    if (hit(B::BTN_RIGHT)) slot_ = (slot_ + 1) % 10;
  }
  if (hit(B::BTN_B)) return Result::Resume;
  if (!hit(B::BTN_A) && !hit(B::BTN_START)) return Result::None;
  if (item == kSlotRow) { page_ = Page::Slot; row_ = slot_; return Result::None; }
  if (item == kCheatRow) {
    build_lines();
    cheat_row_ = 0;
    cheat_top_ = 0;
    if (!lines_.empty() && lines_[0].kind != Line::Toggle) move_cheat_row(+1);
    page_ = Page::Cheats;
    return Result::None;
  }
  return kRoot[item].result;
}

// Truncated to fit, with an ellipsis, so a long name still says which cheat
// it is rather than running under the panel edge.
namespace {
std::string fit(const std::string& text, int scale, int width_px) {
  if (text_width(scale, text.c_str()) <= width_px) return text;
  std::string out = text;
  while (!out.empty() && text_width(scale, (out + "...").c_str()) > width_px) out.pop_back();
  return out + "...";
}
} // namespace

void draw_notice(const Blit& d, const char* title, const char* line2, const char* line3) {
  constexpr int w = 232, h = kTitleY + kGlyphPx * 2 + 6 + kGlyphH + 22;
  const int x0 = (SCREEN_W - w) / 2, y0 = (static_cast<int>(d.h) - h) / 2;
  panel(d, x0, y0, w, h);
  const auto centred = [&](int y, int scale, u32 ink, const char* s) {
    draw_text(d, x0 + (w - text_width(scale, s)) / 2, y, scale, ink, s);
  };
  // Titles run long ("Mario & Luigi - Bowser's Inside Story"): cut to the
  // panel with an ellipsis, as the game list does.
  centred(y0 + kTitleY, kScale, kInk, fit(title, kScale, w - 16).c_str());
  centred(y0 + kTitleY + kGlyphPx + 6, kScale, kInk, line2);
  centred(y0 + kTitleY + kGlyphPx * 2 + 12, 1, kDim, line3);
}

// The cheats page: one scrolling list with the database's own headings in it.
void Menu::draw_cheats(const Blit& d) const {
  const int px0 = (static_cast<int>(ds::SCREEN_W) - kCheatPanelW) / 2;
  const int py0 = (static_cast<int>(ds::SCREEN_H) - kCheatPanelH) / 2;
  panel(d, px0, py0, kCheatPanelW, kCheatPanelH);

  // The heading counts the codes, not the lines, so it matches the number the
  // frontend logged when it loaded them.
  size_t on = 0;
  if (codes_) for (const cheat::Code& c : *codes_) if (c.enabled) ++on;
  char title[32];
  std::snprintf(title, sizeof title, "CHEATS  %zu ON", on);
  draw_text(d, px0 + (kCheatPanelW - text_width(kScale, title)) / 2, py0 + kTitleY, kScale, kInk, title);
  fill_rect(d, px0 + 8, py0 + kRuleY, kCheatPanelW - 16, 1, kEdge);

  if (lines_.empty()) {
    draw_text(d, px0 + 10, py0 + kCheatRowsY + 4, kCheatScale, kDim, "NO CHEATS FOR THIS GAME");
    return;
  }

  const int n = static_cast<int>(lines_.size());
  // move_cheat_row keeps this in range; clamped again because draw must be
  // safe whatever the caller did.
  int top = cheat_top_;
  if (top > n - kCheatVisible) top = n - kCheatVisible;
  if (top < 0) top = 0;

  const int text_x = px0 + 8, avail = kCheatPanelW - 16 - 6;
  marquee_overflow_ = 0;   // set below if the selected row is actually too long
  for (int i = 0; i < kCheatVisible && top + i < n; ++i) {
    const Line& l = lines_[static_cast<size_t>(top + i)];
    const int ry = py0 + kCheatRowsY + i * kCheatRowH;
    const bool sel = top + i == cheat_row_;
    if (sel) fill_rect(d, px0 + 4, ry - 2, kCheatPanelW - 14, kCheatRowH, kSel);
    if (l.kind == Line::Heading) {
      const std::string& name = (*groups_)[static_cast<size_t>(l.at)].name;
      draw_text(d, text_x, ry, kCheatScale, kEdgeText, fit(name, kCheatScale, avail).c_str());
      continue;
    }
    const cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
    if (l.kind == Line::Note) {
      // A note is the database author talking, not a switch: no box for it.
      draw_text(d, text_x + 4, ry, kCheatScale, kDim, fit(c.name, kCheatScale, avail - 4).c_str());
      continue;
    }
    const char* box = c.enabled ? "[X] " : "[ ] ";
    const u32 ink = c.enabled || sel ? kInk : kDim;
    if (!sel) {
      draw_text(d, text_x, ry, kCheatScale, ink, fit(std::string(box) + c.name, kCheatScale, avail).c_str());
      continue;
    }
    // The selected row scrolls its name rather than cutting it, so the whole
    // of it can be read without leaving the row. The checkbox stays put --
    // it is the thing being toggled, and it must not scroll out of sight --
    // so only the name moves, inside what is left of the row.
    const int box_w = text_width(kCheatScale, box) + kCheatScale;
    draw_text(d, text_x, ry, kCheatScale, ink, box);
    const int name_x = text_x + box_w, name_avail = avail - box_w;
    marquee_overflow_ = text_width(kCheatScale, c.name.c_str()) - name_avail;
    if (marquee_overflow_ <= 0) {
      draw_text(d, name_x, ry, kCheatScale, ink, c.name.c_str());
      continue;
    }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = name_x;
    g_clip_x1 = name_x + name_avail;
    draw_text(d, name_x - marquee_offset(marquee_overflow_), ry, kCheatScale, ink, c.name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }

  scroll_bar(d, px0, py0, n, top);
}

// The game picker: the cheats page's list, with a row per ROM and nothing to
// toggle. A long filename is the rule rather than the exception here, so the
// selected row scrolls its name exactly as a long cheat name does.
void Menu::draw_games(const Blit& d) const {
  const int px0 = (static_cast<int>(ds::SCREEN_W) - kCheatPanelW) / 2;
  const int py0 = (static_cast<int>(ds::SCREEN_H) - kCheatPanelH) / 2;
  panel(d, px0, py0, kCheatPanelW, kCheatPanelH);

  const int n = games_ ? static_cast<int>(games_->size()) : 0;
  char title[32];
  std::snprintf(title, sizeof title, "GAMES  %d", n);
  draw_text(d, px0 + (kCheatPanelW - text_width(kScale, title)) / 2, py0 + kTitleY, kScale, kInk, title);
  fill_rect(d, px0 + 8, py0 + kRuleY, kCheatPanelW - 16, 1, kEdge);

  if (n == 0) {
    // Say what is wrong rather than showing an empty box: an unset or empty
    // games directory is the likely reason, and it is fixable.
    draw_text(d, px0 + 10, py0 + kCheatRowsY + 4, kCheatScale, kDim, "NO GAMES FOUND -- SET");
    draw_text(d, px0 + 10, py0 + kCheatRowsY + 4 + kCheatRowH, kCheatScale, kDim, "[PATHS] GAMES IN THE");
    draw_text(d, px0 + 10, py0 + kCheatRowsY + 4 + 2 * kCheatRowH, kCheatScale, kDim, "CONFIG FILE");
    return;
  }

  int top = game_top_;
  if (top > n - kCheatVisible) top = n - kCheatVisible;
  if (top < 0) top = 0;

  const int text_x = px0 + 8, avail = kCheatPanelW - 16 - 6;
  marquee_overflow_ = 0;
  for (int i = 0; i < kCheatVisible && top + i < n; ++i) {
    const std::string& name = (*games_)[static_cast<size_t>(top + i)].title;
    const int ry = py0 + kCheatRowsY + i * kCheatRowH;
    const bool sel = top + i == game_row_;
    if (sel) fill_rect(d, px0 + 4, ry - 2, kCheatPanelW - 14, kCheatRowH, kSel);
    if (!sel) {
      draw_text(d, text_x, ry, kCheatScale, kDim, fit(name, kCheatScale, avail).c_str());
      continue;
    }
    marquee_overflow_ = text_width(kCheatScale, name.c_str()) - avail;
    if (marquee_overflow_ <= 0) { draw_text(d, text_x, ry, kCheatScale, kInk, name.c_str()); continue; }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = text_x;
    g_clip_x1 = text_x + avail;
    draw_text(d, text_x - marquee_offset(marquee_overflow_), ry, kCheatScale, kInk, name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }
  scroll_bar(d, px0, py0, n, top);
}

void Menu::draw(const Blit& d) const {
  if (page_ == Page::Games) { draw_games(d); return; }
  if (page_ == Page::Cheats) { draw_cheats(d); return; }
  const bool slots = page_ == Page::Slot;
  const int scale = kScale, row_h = kRowH, title_y = kTitleY, rule_y = kRuleY, rows_y = kRowsY;
  const int rows = slots ? kSlotRows : root_rows();   // the slot page stacks its ten in two columns
  const int panel_w = slots ? 200 : 150;
  const int panel_h = panel_height(rows);
  const int px0 = (static_cast<int>(ds::SCREEN_W) - panel_w) / 2;
  const int py0 = (static_cast<int>(ds::SCREEN_H) - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);

  const char* title = slots ? "STATE SLOT" : "PAUSED";
  draw_text(d, px0 + (panel_w - text_width(scale, title)) / 2, py0 + title_y, scale, kInk, title);
  fill_rect(d, px0 + 8, py0 + rule_y, panel_w - 16, 1, kEdge);

  char buf[24];
  for (int i = 0; i < (slots ? 10 : root_rows()); ++i) {
    // Slots fill a column at a time: 0-4 on the left, 5-9 on the right.
    const int col = slots ? i / kSlotRows : 0;
    const int cell_w = slots ? (panel_w - 16) / 2 : panel_w - 16;
    const int cell_x = px0 + 8 + col * cell_w;
    const int ry = py0 + rows_y + (slots ? i % kSlotRows : i) * row_h;
    if (i == row_) fill_rect(d, cell_x, ry - 2, cell_w, row_h, kSel);
    const char* label = buf;
    const int item = slots ? i : root_item(i);
    if (slots) std::snprintf(buf, sizeof buf, "%d %s", i, used_[i] ? "USED" : "EMPTY");
    else if (item == kSlotRow) std::snprintf(buf, sizeof buf, "SLOT < %d >", slot_);
    else label = kRoot[item].label;
    // Loading an empty slot, and every empty slot in the list, reads dimmer:
    // the menu says what is there before the player commits to it.
    const bool weak = (slots && !used_[i]) || (!slots && kRoot[item].result == Result::Load && !used_[slot_]);
    draw_text(d, cell_x + 6, ry, scale, weak && i != row_ ? kDim : kInk, label);
  }
}

} // namespace ds::sdl
