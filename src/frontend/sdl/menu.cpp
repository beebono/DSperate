// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"

#include <cstdio>
#include <iterator>

namespace ds::sdl {
namespace {

// 5x7 glyphs, one byte per row, bits 4..0 left to right; ' ' (0x20) to 'Z'.
// Wider than the 3x5 the slot digit uses: at this size M/N/W are distinct and
// a word is read rather than decoded, which a menu needs and an OSD digit
// does not.
constexpr u8 kFont[59][7] = {
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
  {17,17,10, 4, 4, 4, 4}, {31, 1, 2, 4, 8,16,31}};

constexpr int kGlyphW = 5, kGlyphH = 7, kAdvance = 6;   // advance includes the one-pixel gap

// Panel geometry. Rows are `kRowH` tall around a `kGlyphH * kScale` glyph, so
// a selected row's bar sits evenly above and below its text -- derive it, do
// not hand-tune it, or the bar drifts off the text the next time either
// changes. The header keeps its own scale; rows share kScale.
constexpr int kScale = 2, kGlyphPx = kGlyphH * kScale, kRowH = kGlyphPx + 4;
constexpr int kTitleY = 8, kRuleY = kTitleY + kGlyphPx + 6, kRowsY = kRuleY + 8;
constexpr int panel_height(int rows) { return kRowsY + rows * kRowH + 8; }

int glyph(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  const int i = static_cast<int>(static_cast<unsigned char>(c)) - 0x20;
  return (i >= 0 && i < 59) ? i : 0;
}

// One DS pixel, mapped onto the destination. Clipped to the DS screen so a
// caller may lay out past the edges without checking.
void put(const Blit& d, int x, int y, u32 colour) {
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

// The root page, in order. A null label is the slot row: it is formatted from
// the current slot and opens the slot page rather than returning a result.
// New entries go here and nowhere else -- the panel sizes itself to the count.
constexpr int kSlotRow = 2;
constexpr struct RootItem { const char* label; Menu::Result result; } kRoot[] = {
  {"SAVE STATE", Menu::Result::Save},
  {"LOAD STATE", Menu::Result::Load},
  {nullptr,      Menu::Result::None},
  {"RESUME",     Menu::Result::Resume},
  {"QUIT",       Menu::Result::Quit},
};
static_assert(static_cast<int>(std::size(kRoot)) == Menu::kRootRows, "kRootRows must match the table");
static_assert(kRoot[kSlotRow].label == nullptr, "kSlotRow must name the slot row");
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

Menu::Result Menu::input(u32 presses) {
  using B = io::Io::Button;
  const auto hit = [&](B b) { return (presses >> b) & 1; };
  if (page_ == Page::Slot) {
    // Ten slots as two columns of five: up/down walk a column, left/right
    // cross between them. row_ is the slot itself, 0-4 left and 5-9 right.
    if (hit(B::BTN_UP))    row_ = (row_ % kSlotRows == 0) ? row_ + kSlotRows - 1 : row_ - 1;
    if (hit(B::BTN_DOWN))  row_ = (row_ % kSlotRows == kSlotRows - 1) ? row_ - kSlotRows + 1 : row_ + 1;
    if (hit(B::BTN_LEFT) || hit(B::BTN_RIGHT)) row_ = (row_ + kSlotRows) % 10;
    if (hit(B::BTN_B)) { page_ = Page::Root; row_ = 2; return Result::None; }
    if (hit(B::BTN_A) || hit(B::BTN_START)) { slot_ = row_; page_ = Page::Root; row_ = 2; }
    return Result::None;
  }
  if (hit(B::BTN_UP))   row_ = (row_ + kRootRows - 1) % kRootRows;
  if (hit(B::BTN_DOWN)) row_ = (row_ + 1) % kRootRows;
  // Left/right are a shortcut on the slot row, so the common adjustment does
  // not need the page at all.
  if (row_ == kSlotRow) {
    if (hit(B::BTN_LEFT))  slot_ = (slot_ + 9) % 10;
    if (hit(B::BTN_RIGHT)) slot_ = (slot_ + 1) % 10;
  }
  if (hit(B::BTN_B)) return Result::Resume;
  if (!hit(B::BTN_A) && !hit(B::BTN_START)) return Result::None;
  if (row_ == kSlotRow) { page_ = Page::Slot; row_ = slot_; return Result::None; }
  return kRoot[row_].result;
}

void Menu::draw(const Blit& d) const {
  const bool slots = page_ == Page::Slot;
  const int scale = kScale, row_h = kRowH, title_y = kTitleY, rule_y = kRuleY, rows_y = kRowsY;
  const int rows = slots ? kSlotRows : kRootRows;   // the slot page stacks its ten in two columns
  const int panel_w = slots ? 200 : 150;
  const int panel_h = panel_height(rows);
  const int px0 = (static_cast<int>(ds::SCREEN_W) - panel_w) / 2;
  const int py0 = (static_cast<int>(ds::SCREEN_H) - panel_h) / 2;
  fill_rect(d, px0, py0, panel_w, panel_h, kPanel);
  fill_rect(d, px0, py0, panel_w, 1, kEdge);
  fill_rect(d, px0, py0 + panel_h - 1, panel_w, 1, kEdge);
  fill_rect(d, px0, py0, 1, panel_h, kEdge);
  fill_rect(d, px0 + panel_w - 1, py0, 1, panel_h, kEdge);

  const char* title = slots ? "STATE SLOT" : "PAUSED";
  draw_text(d, px0 + (panel_w - text_width(scale, title)) / 2, py0 + title_y, scale, kInk, title);
  fill_rect(d, px0 + 8, py0 + rule_y, panel_w - 16, 1, kEdge);

  char buf[24];
  for (int i = 0; i < (slots ? 10 : kRootRows); ++i) {
    // Slots fill a column at a time: 0-4 on the left, 5-9 on the right.
    const int col = slots ? i / kSlotRows : 0;
    const int cell_w = slots ? (panel_w - 16) / 2 : panel_w - 16;
    const int cell_x = px0 + 8 + col * cell_w;
    const int ry = py0 + rows_y + (slots ? i % kSlotRows : i) * row_h;
    if (i == row_) fill_rect(d, cell_x, ry - 2, cell_w, row_h, kSel);
    const char* label = buf;
    if (slots) std::snprintf(buf, sizeof buf, "%d %s", i, used_[i] ? "USED" : "EMPTY");
    else if (i == kSlotRow) std::snprintf(buf, sizeof buf, "SLOT < %d >", slot_);
    else label = kRoot[i].label;
    // Loading an empty slot, and every empty slot in the list, reads dimmer:
    // the menu says what is there before the player commits to it.
    const bool weak = (slots && !used_[i]) || (!slots && kRoot[i].result == Result::Load && !used_[slot_]);
    draw_text(d, cell_x + 6, ry, scale, weak && i != row_ ? kDim : kInk, label);
  }
}

} // namespace ds::sdl
