// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"

#include <algorithm>
#include <cmath>
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

// Panel geometry, derived at draw time from the canvas rather than fixed for a
// 256x192 one. Rows are `row_h` tall around a `glyph_px` glyph, so a selected
// row's bar sits evenly above and below its text -- derive it, do not
// hand-tune it, or the bar drifts off the text the next time either changes.
//
// Every measure is a multiple of the glyph scale, so at scale 2 -- which is
// what a 256x192 canvas gives, the size the menu had when it was drawn into a
// DS framebuffer -- these come out at exactly the old constants. That is
// deliberate: the DS-space fallback path draws the same pixels it always did.
struct Metrics {
  int s;            // glyph scale for ordinary rows
  int list_s;       // ... and for the scrolling list pages
  int glyph_px, row_h, title_y, rule_y, rows_y, pad;
  int list_row_h, list_rows_y;
};

Metrics metrics(const Canvas& d) {
  Metrics m{};
  m.s = ui_scale(d);
  // Cheat names are sentences ("Press L+R+SELECT For 7 Red Coins") and a game
  // can have thousands of them, so the list pages drop to half scale: about
  // thirty characters across and twelve at a time, against fifteen and five.
  m.list_s = std::max(1, m.s / 2);
  m.glyph_px = kGlyphH * m.s;
  m.row_h    = m.glyph_px + 2 * m.s;
  m.title_y  = 4 * m.s;
  m.rule_y   = m.title_y + m.glyph_px + 3 * m.s;
  m.rows_y   = m.rule_y + 4 * m.s;
  m.pad      = 4 * m.s;
  m.list_row_h  = kGlyphH * m.list_s + 4 * m.list_s;
  m.list_rows_y = m.rule_y + 3 * m.s;
  return m;
}

int panel_height(const Metrics& m, int rows) { return m.rows_y + rows * m.row_h + m.pad; }
// The list pages take most of the canvas: they are the ones with thousands of
// entries, and rows they cannot show are rows the player has to scroll to.
int list_panel_w(const Canvas& d, const Metrics& m) { return std::min(d.w - m.pad, 120 * m.s); }
int list_panel_h(const Canvas& d, const Metrics& m) { return std::min(d.h - m.pad, 89 * m.s); }
int list_visible(const Metrics& m, int panel_h) {
  return std::max(1, (panel_h - m.list_rows_y - m.pad) / m.list_row_h);
}

int glyph(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  const int i = static_cast<int>(static_cast<unsigned char>(c)) - 0x20;
  return (i >= 0 && i < 62) ? i : 0;
}

// One pixel. Clipped to the canvas so a caller may lay out past the edges
// without checking. Columns outside [clip_x0, clip_x1) are dropped, which is
// what lets a name scroll under the panel edge instead of over it; the
// default admits everything, and draw sets it around a scrolling row.
int g_clip_x0 = 0, g_clip_x1 = 1 << 30;

void put(const Canvas& d, int x, int y, u32 colour) {
  if (x < g_clip_x0 || x >= g_clip_x1) return;
  if (x < 0 || x >= d.w || y < 0 || y >= d.h) return;
  d.px[static_cast<size_t>(y) * d.pitch + static_cast<size_t>(x)] = colour;
}

// Clipped once around the whole span rather than per pixel: a filled row is
// the commonest thing drawn and the panel is mostly fill.
void fill_rect(const Canvas& d, int x, int y, int w, int h, u32 colour) {
  const int x0 = std::max({x, 0, g_clip_x0}), x1 = std::min({x + w, d.w, g_clip_x1});
  const int y0 = std::max(y, 0), y1 = std::min(y + h, d.h);
  for (int yy = y0; yy < y1; ++yy) {
    u32* row = d.px + static_cast<size_t>(yy) * d.pitch;
    for (int xx = x0; xx < x1; ++xx) row[xx] = colour;
  }
}

constexpr u32 kInk = 0xFFFFFFFF, kDim = 0xFF909090, kPanel = 0xFF101018, kEdge = 0xFF5060A0, kSel = 0xFF3050A0;
constexpr u32 kEdgeText = 0xFFA0B0E0, kPanelEdgeDim = 0xFF303040;   // group headings; the scroll-bar track

// The panel every page sits in: a filled box with a one-pixel edge.
void panel(const Canvas& d, int x, int y, int w, int h) {
  fill_rect(d, x, y, w, h, kPanel);
  fill_rect(d, x, y, w, 1, kEdge);
  fill_rect(d, x, y + h - 1, w, 1, kEdge);
  fill_rect(d, x, y, 1, h, kEdge);
  fill_rect(d, x + w - 1, y, 1, h, kEdge);
}

// A scroll bar for a list page, because the list gives no other clue how long
// it is: some games have five thousand cheats, and a library can be as long.
// Drawn only when there is something off screen.
void scroll_bar(const Canvas& d, const Metrics& m, int px0, int py0, int panel_w, int visible, int n, int top) {
  if (n <= visible) return;
  const int w = std::max(2, m.list_s * 2);
  const int track_x = px0 + panel_w - w - m.list_s * 3, track_y = py0 + m.list_rows_y - m.list_s * 2;
  const int track_h = visible * m.list_row_h;
  fill_rect(d, track_x, track_y, w, track_h, kPanelEdgeDim);
  int bar = track_h * visible / n;
  if (bar < 4 * m.list_s) bar = 4 * m.list_s;
  const int span = track_h - bar;
  fill_rect(d, track_x, track_y + (span > 0 ? span * top / (n - visible) : 0), w, bar, kEdge);
}

// The root page, in order. A null label is the slot row: it is formatted from
// the current slot and opens the slot page rather than returning a result.
// New entries go here and nowhere else -- the panel sizes itself to the count.
constexpr int kSlotRow = 2;
constexpr int kCheatRow = 3;   // hidden when no database matched this ROM
constexpr int kOptionsRow = 4; // hidden when the frontend gave no settings host
constexpr struct RootItem { const char* label; Menu::Result result; } kRoot[] = {
  {"SAVE STATE", Menu::Result::Save},
  {"LOAD STATE", Menu::Result::Load},
  {nullptr,      Menu::Result::None},
  {"CHEATS",     Menu::Result::None},
  {"OPTIONS",    Menu::Result::None},
  {"RESUME",     Menu::Result::Resume},
  {"QUIT",       Menu::Result::Quit},
};
static_assert(static_cast<int>(std::size(kRoot)) == Menu::kRootRows, "kRootRows must match the table");
static_assert(kRoot[kOptionsRow].result == Menu::Result::None, "kOptionsRow must name the options row");
static_assert(kRoot[kSlotRow].label == nullptr, "kSlotRow must name the slot row");
static_assert(kRoot[kCheatRow].result == Menu::Result::None, "kCheatRow must name the cheats row");
// Rows are cheap to add (one entry above). The panel is sized from the canvas
// at draw time and the glyph scale steps down if it would not fit, so a new
// row costs height rather than clipping in silence the way `put` would.

} // namespace

int text_width(int scale, const char* s) {
  int n = 0;
  for (const char* p = s; *p; ++p) ++n;
  return n > 0 ? n * kAdvance * scale - scale : 0;   // no gap after the last glyph
}

int draw_text(const Canvas& d, int x, int y, int scale, u32 colour, const char* s) {
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

int ui_scale(const Canvas& d) {
  // Twice the canvas's reduction against a DS screen: the menu used to draw at
  // scale 2 into a 256x192 buffer that was then upscaled to the panel, so this
  // is the same size on the glass. A 256x192 canvas -- the DS-space fallback --
  // therefore lands back on exactly 2.
  const double r = std::min(static_cast<double>(d.w) / ds::SCREEN_W, static_cast<double>(d.h) / ds::SCREEN_H);
  const int s = static_cast<int>(std::lround(2.0 * r));
  return std::clamp(s, 2, 10);
}

void dim_framebuffer(u32* px, u32 n) {
  for (u32 i = 0; i < n; ++i) px[i] = 0xFF000000 | ((px[i] >> 1) & 0x007F7F7F);
}



void Menu::set_open(bool o) {
  open_ = o;
  depth_ = 1;
  stack_[0] = Page::Root;
  row_ = 0;
}

void Menu::set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) {
  codes_ = codes;
  groups_ = groups;
  lines_.clear();
}

// The root page hides the rows that would have nothing behind them -- cheats
// when no database matched, options when the frontend gave no host -- so what
// is on screen is a subset of the table and the two have to be mapped.
bool Menu::root_visible(int item) const {
  if (item == kCheatRow) return have_cheats();
  if (item == kOptionsRow) return have_options();
  return true;
}

int Menu::root_rows() const {
  int n = 0;
  for (int i = 0; i < kRootRows; ++i) if (root_visible(i)) ++n;
  return n;
}

int Menu::root_item(int row) const {
  for (int i = 0; i < kRootRows; ++i) if (root_visible(i) && row-- == 0) return i;
  return kRootRows - 1;
}

void Menu::push(Page p) {
  if (depth_ < kMaxDepth) stack_[depth_++] = p;
}

bool Menu::pop() {
  if (depth_ <= 1) return false;
  --depth_;
  return true;
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
      if (cheat_row_ >= cheat_top_ + visible_) cheat_top_ = cheat_row_ - visible_ + 1;
      if (cheat_top_ > n - visible_) cheat_top_ = n - visible_;
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
  depth_ = 1;
  stack_[0] = Page::Games;
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
  if (game_row_ >= game_top_ + visible_) game_top_ = game_row_ - visible_ + 1;
  if (game_top_ > n - visible_) game_top_ = n - visible_;
  if (game_top_ < 0) game_top_ = 0;
}

int Menu::list_row() const {
  if (page() == Page::Games) return game_row_;
  if (settings_page()) return set_row_[table_slot()];
  return cheat_row_;
}

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
        if (page() == Page::Games) move_game_row(dir);
        else if (settings_page()) move_setting_row(dir);
        else move_cheat_row(dir);
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
  if (page() == Page::Slot) {
    // Ten slots as two columns of five: up/down walk a column, left/right
    // cross between them. slot_row_ is the slot itself, 0-4 left and 5-9
    // right. It is not row_, which is the root page's selection: they used to
    // share one variable and be reset on every entry and exit, which a page
    // stack cannot do -- popping has to leave the page below as it was.
    if (hit(B::BTN_UP))    slot_row_ = (slot_row_ % kSlotRows == 0) ? slot_row_ + kSlotRows - 1 : slot_row_ - 1;
    if (hit(B::BTN_DOWN))  slot_row_ = (slot_row_ % kSlotRows == kSlotRows - 1) ? slot_row_ - kSlotRows + 1 : slot_row_ + 1;
    if (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT)) slot_row_ = (slot_row_ + kSlotRows) % 10;
    if (hit(B::BTN_B)) { pop(); return Result::None; }
    if (hit(B::BTN_A) || hit(B::BTN_START)) { slot_ = slot_row_; pop(); }
    return Result::None;
  }
  if (page() == Page::Games) {
    // The same walk as the cheats page: up/down step, the shoulders page.
    // A is the only way off it -- see open_games() on why B is inert.
    if (hit(B::BTN_UP))   move_game_row(-1);
    if (hit(B::BTN_DOWN)) move_game_row(+1);
    if (hit(B::BTN_L)) move_game_row(-visible_);
    if (hit(B::BTN_R)) move_game_row(+visible_);
    if ((hit(B::BTN_A) || hit(B::BTN_START)) && games_ && !games_->empty()) {
      chosen_ = (*games_)[static_cast<size_t>(game_row_)].path;
      return Result::Launch;
    }
    return Result::None;
  }
  if (page() == Page::Options) return handle_options(presses);
  if (settings_page()) return handle_settings(presses);
  if (page() == Page::Cheats) {
    // Up/down step, the shoulders page: a list of thousands is not one to
    // walk a row at a time.
    if (hit(B::BTN_UP))   move_cheat_row(-1);
    if (hit(B::BTN_DOWN)) move_cheat_row(+1);
    if (hit(B::BTN_L)) for (int i = 0; i < visible_; ++i) move_cheat_row(-1);
    if (hit(B::BTN_R)) for (int i = 0; i < visible_; ++i) move_cheat_row(+1);
    if (hit(B::BTN_A)) toggle_cheat();
    if (hit(B::BTN_B)) pop();
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
  if (item == kSlotRow) { push(Page::Slot); slot_row_ = slot_; return Result::None; }
  if (item == kCheatRow) {
    build_lines();
    cheat_row_ = 0;
    cheat_top_ = 0;
    if (!lines_.empty() && lines_[0].kind != Line::Toggle) move_cheat_row(+1);
    push(Page::Cheats);
    return Result::None;
  }
  if (item == kOptionsRow) { push(Page::Options); return Result::None; }
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

void draw_notice(const Canvas& d, const char* title, const char* line2, const char* line3) {
  const Metrics m = metrics(d);
  const int w = std::min(d.w - 2 * m.pad, 116 * m.s);
  const int h = m.title_y + m.glyph_px * 2 + 3 * m.s + kGlyphH * m.list_s + 11 * m.s;
  const int x0 = (d.w - w) / 2, y0 = (d.h - h) / 2;
  panel(d, x0, y0, w, h);
  const auto centred = [&](int y, int scale, u32 ink, const char* s) {
    draw_text(d, x0 + (w - text_width(scale, s)) / 2, y, scale, ink, s);
  };
  // Titles run long ("Mario & Luigi - Bowser's Inside Story"): cut to the
  // panel with an ellipsis, as the game list does.
  centred(y0 + m.title_y, m.s, kInk, fit(title, m.s, w - 8 * m.s).c_str());
  centred(y0 + m.title_y + m.glyph_px + 3 * m.s, m.s, kInk, line2);
  centred(y0 + m.title_y + m.glyph_px * 2 + 6 * m.s, m.list_s, kDim, line3);
}

// The two scrolling pages share their frame: panel, centred title, rule, and
// the geometry every row is laid out against. `visible_` is measured here
// because update() needs it and only draw knows the canvas.
namespace {
struct ListFrame { Metrics m; int px0, py0, w, h, visible, text_x, avail; };

ListFrame list_frame(const Canvas& d, const char* title) {
  ListFrame f{};
  f.m = metrics(d);
  f.w = list_panel_w(d, f.m);
  f.h = list_panel_h(d, f.m);
  f.px0 = (d.w - f.w) / 2;
  f.py0 = (d.h - f.h) / 2;
  f.visible = list_visible(f.m, f.h);
  panel(d, f.px0, f.py0, f.w, f.h);
  draw_text(d, f.px0 + (f.w - text_width(f.m.s, title)) / 2, f.py0 + f.m.title_y, f.m.s, kInk, title);
  fill_rect(d, f.px0 + f.m.pad, f.py0 + f.m.rule_y, f.w - 2 * f.m.pad, std::max(1, f.m.s / 2), kEdge);
  // The scroll bar's track sits in the right margin, so rows stop short of it.
  f.text_x = f.px0 + f.m.pad;
  f.avail = f.w - 2 * f.m.pad - 6 * f.m.list_s;
  return f;
}
} // namespace

// The cheats page: one scrolling list with the database's own headings in it.
void Menu::draw_cheats(const Canvas& d) const {
  // The heading counts the codes, not the lines, so it matches the number the
  // frontend logged when it loaded them.
  size_t on = 0;
  if (codes_) for (const cheat::Code& c : *codes_) if (c.enabled) ++on;
  char title[32];
  std::snprintf(title, sizeof title, "CHEATS  %zu ON", on);
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  visible_ = f.visible;

  if (lines_.empty()) {
    draw_text(d, f.text_x + m.list_s * 2, f.py0 + m.list_rows_y + m.list_s * 4, m.list_s, kDim, "NO CHEATS FOR THIS GAME");
    return;
  }

  const int n = static_cast<int>(lines_.size());
  // move_cheat_row keeps this in range; clamped again because draw must be
  // safe whatever the caller did.
  int top = cheat_top_;
  if (top > n - f.visible) top = n - f.visible;
  if (top < 0) top = 0;

  marquee_overflow_ = 0;   // set below if the selected row is actually too long
  for (int i = 0; i < f.visible && top + i < n; ++i) {
    const Line& l = lines_[static_cast<size_t>(top + i)];
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool sel = top + i == cheat_row_;
    if (sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    if (l.kind == Line::Heading) {
      const std::string& name = (*groups_)[static_cast<size_t>(l.at)].name;
      draw_text(d, f.text_x, ry, m.list_s, kEdgeText, fit(name, m.list_s, f.avail).c_str());
      continue;
    }
    const cheat::Code& c = (*codes_)[static_cast<size_t>(l.at)];
    if (l.kind == Line::Note) {
      // A note is the database author talking, not a switch: no box for it.
      draw_text(d, f.text_x + 4 * m.list_s, ry, m.list_s, kDim, fit(c.name, m.list_s, f.avail - 4 * m.list_s).c_str());
      continue;
    }
    const char* box = c.enabled ? "[X] " : "[ ] ";
    const u32 ink = c.enabled || sel ? kInk : kDim;
    if (!sel) {
      draw_text(d, f.text_x, ry, m.list_s, ink, fit(std::string(box) + c.name, m.list_s, f.avail).c_str());
      continue;
    }
    // The selected row scrolls its name rather than cutting it, so the whole
    // of it can be read without leaving the row. The checkbox stays put --
    // it is the thing being toggled, and it must not scroll out of sight --
    // so only the name moves, inside what is left of the row.
    const int box_w = text_width(m.list_s, box) + m.list_s;
    draw_text(d, f.text_x, ry, m.list_s, ink, box);
    const int name_x = f.text_x + box_w, name_avail = f.avail - box_w;
    marquee_overflow_ = text_width(m.list_s, c.name.c_str()) - name_avail;
    if (marquee_overflow_ <= 0) {
      draw_text(d, name_x, ry, m.list_s, ink, c.name.c_str());
      continue;
    }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = name_x;
    g_clip_x1 = name_x + name_avail;
    draw_text(d, name_x - marquee_offset(marquee_overflow_), ry, m.list_s, ink, c.name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }

  scroll_bar(d, m, f.px0, f.py0, f.w, f.visible, n, top);
}

// The game picker: the cheats page's list, with a row per ROM and nothing to
// toggle. A long filename is the rule rather than the exception here, so the
// selected row scrolls its name exactly as a long cheat name does.
void Menu::draw_games(const Canvas& d) const {
  const int n = games_ ? static_cast<int>(games_->size()) : 0;
  char title[32];
  std::snprintf(title, sizeof title, "GAMES  %d", n);
  const ListFrame f = list_frame(d, title);
  const Metrics& m = f.m;
  visible_ = f.visible;

  if (n == 0) {
    // Say what is wrong rather than showing an empty box: an unset or empty
    // games directory is the likely reason, and it is fixable.
    const char* why[] = {"NO GAMES FOUND -- SET", "[PATHS] GAMES IN THE", "CONFIG FILE"};
    for (int i = 0; i < 3; ++i)
      draw_text(d, f.text_x + m.list_s * 2, f.py0 + m.list_rows_y + m.list_s * 4 + i * m.list_row_h, m.list_s, kDim, why[i]);
    return;
  }

  int top = game_top_;
  if (top > n - f.visible) top = n - f.visible;
  if (top < 0) top = 0;

  marquee_overflow_ = 0;
  for (int i = 0; i < f.visible && top + i < n; ++i) {
    const std::string& name = (*games_)[static_cast<size_t>(top + i)].title;
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool sel = top + i == game_row_;
    if (sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    if (!sel) {
      draw_text(d, f.text_x, ry, m.list_s, kDim, fit(name, m.list_s, f.avail).c_str());
      continue;
    }
    marquee_overflow_ = text_width(m.list_s, name.c_str()) - f.avail;
    if (marquee_overflow_ <= 0) { draw_text(d, f.text_x, ry, m.list_s, kInk, name.c_str()); continue; }
    const int clip0 = g_clip_x0, clip1 = g_clip_x1;
    g_clip_x0 = f.text_x;
    g_clip_x1 = f.text_x + f.avail;
    draw_text(d, f.text_x - marquee_offset(marquee_overflow_), ry, m.list_s, kInk, name.c_str());
    g_clip_x0 = clip0;
    g_clip_x1 = clip1;
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, f.visible, n, top);
}

// The Options tree. Four pages of settings and, when there is a game in the
// slot, where a change is remembered: the global file, or this game's own.
const Setting* Menu::table() const {
  switch (page()) {
  case Page::Emulation: return kEmuSettings;
  case Page::VisualFx:  return kVideoSettings;
  case Page::Layout:    return kLayoutSettings;
  default:              return kEmuSettings;
  }
}

int Menu::table_slot() const {
  switch (page()) {
  case Page::VisualFx: return 1;
  case Page::Layout:   return 2;
  default:             return 0;
  }
}

int Menu::settings_rows() const { return settings_count(table()); }

// Walks past the rows the host has switched off, exactly as the cheats list
// walks past its headings, and stops at the ends rather than wrapping.
bool Menu::move_setting_row(int delta) {
  host_->commit();   // done with the row that is being left
  const Setting* t = table();
  const int n = settings_count(t);
  const int slot = table_slot();
  int at = set_row_[slot];
  for (int step = 0; step < n; ++step) {
    at += delta > 0 ? 1 : -1;
    if (at < 0 || at >= n) return false;
    if (!host_->enabled(t[at])) continue;
    set_row_[slot] = at;
    return true;
  }
  return false;
}

void Menu::step_setting(int dir) {
  const Setting* t = table();
  const Setting& s = t[set_row_[table_slot()]];
  if (!host_->enabled(s)) return;
  const std::string cur = host_->get(s.key);
  const std::string next = step_value(s, cur, dir, *host_);
  if (next == cur) return;
  host_->set(s.key, next);
}

Menu::Result Menu::handle_options(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  const int rows = 4 + (host_->has_game() ? 1 : 0);
  if (hit(B::BTN_UP))   opt_row_ = (opt_row_ + rows - 1) % rows;
  if (hit(B::BTN_DOWN)) opt_row_ = (opt_row_ + 1) % rows;
  const bool save_row = host_->has_game() && opt_row_ == 4;
  // Left/right work the save-to switch in place, the way they work the slot
  // on the root page: it is a two-way choice, not a page to enter.
  if (save_row && (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT) || hit(B::BTN_A) || hit(B::BTN_START)))
    host_->set_save_per_game(!host_->save_per_game());
  if (hit(B::BTN_B)) { pop(); return Result::None; }
  if (save_row || (!hit(B::BTN_A) && !hit(B::BTN_START))) return Result::None;
  static constexpr Page kPages[4] = {Page::Emulation, Page::VisualFx, Page::Layout, Page::Emulation};
  if (opt_row_ == 3) return Result::None;   // DS OPTIONS: not built yet
  push(kPages[opt_row_]);
  // Land on something selectable: the first row of a page can be switched off
  // (the layout page's pip rows in a stacked layout, say).
  const int slot = table_slot();
  set_row_[slot] = 0;
  if (!host_->enabled(table()[0]) && !move_setting_row(+1)) set_row_[slot] = 0;
  return Result::None;
}

Menu::Result Menu::handle_settings(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (hit(B::BTN_UP))    move_setting_row(-1);
  if (hit(B::BTN_DOWN))  move_setting_row(+1);
  if (hit(B::BTN_LEFT))  step_setting(-1);
  if (hit(B::BTN_RIGHT)) step_setting(+1);
  // A steps a setting forward as well, so the whole page can be worked with
  // one button on a handheld whose d-pad the player is already holding.
  if (hit(B::BTN_A))     step_setting(+1);
  if (hit(B::BTN_B))   { host_->commit(); pop(); }
  return Result::None;
}

void Menu::draw_options(const Canvas& d) const {
  static constexpr const char* kItems[4] = {"EMULATION", "VISUAL FX", "LAYOUT", "DS OPTIONS"};
  Metrics m = metrics(d);
  const int rows = 4 + (host_ && host_->has_game() ? 1 : 0);
  const bool per_game = host_ && host_->save_per_game();
  char save_row[40];
  std::snprintf(save_row, sizeof save_row, "SAVE TO < %s >", per_game ? "THIS GAME" : "GLOBAL");
  // Wide enough for the longest row it will actually draw, rather than a
  // number that happened to fit the rows it had when it was written: the
  // save-to switch changes width as it is toggled, and a panel sized for the
  // shorter one clips the other.
  int widest = 0;
  for (int i = 0; i < 4; ++i) widest = std::max(widest, text_width(m.s, kItems[i]));
  if (rows > 4) widest = std::max(widest, text_width(m.s, save_row));
  // Step the scale down rather than clip, as the root page does.
  while (m.s > 2 && (widest + 8 * m.s > d.w || panel_height(m, rows) > d.h)) {
    m = metrics(Canvas{d.px, d.pitch, d.w * (m.s - 1) / m.s, d.h * (m.s - 1) / m.s});
    widest = 0;
    for (int i = 0; i < 4; ++i) widest = std::max(widest, text_width(m.s, kItems[i]));
    if (rows > 4) { std::snprintf(save_row, sizeof save_row, "SAVE TO < %s >", per_game ? "THIS GAME" : "GLOBAL"); widest = std::max(widest, text_width(m.s, save_row)); }
  }
  const int panel_w = std::min(d.w - 2 * m.pad, widest + 8 * m.s);
  const int panel_h = panel_height(m, rows);
  const int px0 = (d.w - panel_w) / 2, py0 = (d.h - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);
  draw_text(d, px0 + (panel_w - text_width(m.s, "OPTIONS")) / 2, py0 + m.title_y, m.s, kInk, "OPTIONS");
  fill_rect(d, px0 + m.pad, py0 + m.rule_y, panel_w - 2 * m.pad, std::max(1, m.s / 2), kEdge);
  for (int i = 0; i < rows; ++i) {
    const int ry = py0 + m.rows_y + i * m.row_h;
    if (i == opt_row_) fill_rect(d, px0 + m.pad, ry - m.s, panel_w - 2 * m.pad, m.row_h, kSel);
    if (i < 4) {
      // DS OPTIONS has no page behind it yet; it reads dim so that pressing A
      // on it and getting nothing is not a surprise.
      const bool ready = i != 3;
      draw_text(d, px0 + m.pad + 3 * m.s, ry, m.s, ready || i == opt_row_ ? kInk : kDim, kItems[i]);
      continue;
    }
    // The save-to switch, drawn as the slot row is: the value between arrows,
    // so it reads as something to change rather than somewhere to go.
    draw_text(d, px0 + m.pad + 3 * m.s, ry, m.s, kInk, save_row);
  }
}

// A settings page: one scrolling list of label-and-value rows, laid out like
// the cheats page because it is the same problem -- more rows than fit.
void Menu::draw_settings(const Canvas& d) const {
  static constexpr const char* kTitles[3] = {"EMULATION", "VISUAL FX", "LAYOUT"};
  const ListFrame f = list_frame(d, kTitles[table_slot()]);
  const Metrics& m = f.m;
  visible_ = f.visible;
  const Setting* t = table();
  const int n = settings_count(t);
  const int sel = set_row_[table_slot()];

  // A note under the list explains the selected row. It costs two rows of
  // list, and is worth them: these settings are not self-explanatory, and the
  // alternative is the player guessing or reading the ini.
  const int note_rows = 2;
  const int visible = std::max(1, f.visible - note_rows);
  int top = set_top_[table_slot()];
  if (sel < top) top = sel;
  if (sel >= top + visible) top = sel - visible + 1;
  if (top > n - visible) top = n - visible;
  if (top < 0) top = 0;
  set_top_[table_slot()] = top;

  const int value_w = f.avail / 2;
  for (int i = 0; i < visible && top + i < n; ++i) {
    const Setting& s = t[top + i];
    const int ry = f.py0 + m.list_rows_y + i * m.list_row_h;
    const bool on = host_->enabled(s);
    const bool is_sel = top + i == sel;
    if (is_sel) fill_rect(d, f.px0 + m.list_s * 4, ry - m.list_s * 2, f.w - m.list_s * 14, m.list_row_h, kSel);
    // A setting that trades accuracy for speed is coloured, not just noted:
    // the ini's comments shout about these and the menu should too.
    u32 ink = kInk;
    if (!on) ink = kPanelEdgeDim;
    else if (!is_sel && (s.flags & FlagInexact)) ink = kEdgeText;
    draw_text(d, f.text_x, ry, m.list_s, ink, fit(s.label, m.list_s, f.avail - value_w).c_str());
    const std::string v = on ? display_value(s, host_->get(s.key)) : "--";
    const std::string shown = on && is_sel ? "< " + v + " >" : v;
    draw_text(d, f.text_x + f.avail - std::min(value_w, text_width(m.list_s, shown.c_str())),
              ry, m.list_s, ink, fit(shown, m.list_s, value_w).c_str());
  }
  scroll_bar(d, m, f.px0, f.py0, f.w, visible, n, top);

  // The note, under a rule at the foot of the panel.
  const int note_y = f.py0 + m.list_rows_y + visible * m.list_row_h + m.list_s;
  fill_rect(d, f.px0 + m.pad, note_y, f.w - 2 * m.pad, std::max(1, m.list_s), kPanelEdgeDim);
  const Setting& cur = t[sel];
  const char* why = host_->disabled_reason(cur);
  const char* line = why && *why ? why : cur.note;
  const u32 note_ink = why && *why ? kEdgeText : kDim;
  // What it will take to see the change, when that is not "nothing". It takes
  // the second note line, so the note itself wraps into one line when there is
  // one and two when the row is free.
  const char* when = (cur.flags & FlagRestart) ? "RESTART REQUIRED"
                   : (cur.flags & FlagReopen)  ? "REOPENS THE DISPLAY" : nullptr;
  if (when && !host_->enabled(cur)) when = nullptr;
  if (line) {
    const int lines = when ? 1 : note_rows;
    // Wrapped on spaces rather than cut with an ellipsis: a note that stops
    // mid-sentence tells the player less than the room allows.
    std::string rest = line;
    for (int i = 0; i < lines && !rest.empty(); ++i) {
      std::string take = rest;
      size_t cut = std::string::npos;
      while (text_width(m.list_s, take.c_str()) > f.avail) {
        cut = take.find_last_of(' ');
        if (cut == std::string::npos) break;
        take.resize(cut);
      }
      // One word longer than the row: cut it rather than loop forever.
      if (text_width(m.list_s, take.c_str()) > f.avail) take = fit(take, m.list_s, f.avail);
      draw_text(d, f.text_x, note_y + 2 * m.list_s + i * m.list_row_h, m.list_s, note_ink, take.c_str());
      if (take.size() >= rest.size()) break;
      rest.erase(0, take.size());
      while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
    }
  }
  if (when) draw_text(d, f.text_x, note_y + 2 * m.list_s + (note_rows - 1) * m.list_row_h, m.list_s, kEdgeText, when);
}

void Menu::draw(const Canvas& d) const {
  if (page() == Page::Games) { draw_games(d); return; }
  if (page() == Page::Cheats) { draw_cheats(d); return; }
  if (page() == Page::Options) { draw_options(d); return; }
  if (settings_page()) { draw_settings(d); return; }
  const bool slots = page() == Page::Slot;
  Metrics m = metrics(d);
  const int rows = slots ? kSlotRows : root_rows();   // the slot page stacks its ten in two columns
  // Step the glyph scale down rather than let a tall page run off a short
  // canvas: `put` would clip it in silence, which is how the old fixed
  // geometry failed. Two rows always fit at scale 2 on any canvas this runs on.
  while (m.s > 2 && (panel_height(m, rows) > d.h || (slots ? 100 : 75) * m.s > d.w)) m = metrics(Canvas{d.px, d.pitch, d.w * (m.s - 1) / m.s, d.h * (m.s - 1) / m.s});
  const int scale = m.s, row_h = m.row_h, title_y = m.title_y, rule_y = m.rule_y, rows_y = m.rows_y;
  const int panel_w = std::min(d.w - 2 * m.pad, (slots ? 100 : 75) * m.s);
  const int panel_h = panel_height(m, rows);
  const int px0 = (d.w - panel_w) / 2;
  const int py0 = (d.h - panel_h) / 2;
  panel(d, px0, py0, panel_w, panel_h);

  const char* title = slots ? "STATE SLOT" : "PAUSED";
  draw_text(d, px0 + (panel_w - text_width(scale, title)) / 2, py0 + title_y, scale, kInk, title);
  fill_rect(d, px0 + m.pad, py0 + rule_y, panel_w - 2 * m.pad, std::max(1, m.s / 2), kEdge);

  char buf[24];
  for (int i = 0; i < (slots ? 10 : root_rows()); ++i) {
    // Slots fill a column at a time: 0-4 on the left, 5-9 on the right.
    const int col = slots ? i / kSlotRows : 0;
    const int cell_w = slots ? (panel_w - 2 * m.pad) / 2 : panel_w - 2 * m.pad;
    const int cell_x = px0 + m.pad + col * cell_w;
    const int ry = py0 + rows_y + (slots ? i % kSlotRows : i) * row_h;
    if (i == (slots ? slot_row_ : row_)) fill_rect(d, cell_x, ry - m.s, cell_w, row_h, kSel);
    const char* label = buf;
    const int item = slots ? i : root_item(i);
    if (slots) std::snprintf(buf, sizeof buf, "%d %s", i, used_[i] ? "USED" : "EMPTY");
    else if (item == kSlotRow) std::snprintf(buf, sizeof buf, "SLOT < %d >", slot_);
    else label = kRoot[item].label;
    // Loading an empty slot, and every empty slot in the list, reads dimmer:
    // the menu says what is there before the player commits to it.
    const bool weak = (slots && !used_[i]) || (!slots && kRoot[item].result == Result::Load && !used_[slot_]);
    draw_text(d, cell_x + 3 * m.s, ry, scale, weak && i != (slots ? slot_row_ : row_) ? kDim : kInk, label);
  }
}

} // namespace ds::sdl
