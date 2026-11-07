// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The menu's fallback glyphs: every character its 5x7 font lacks (kana, CJK,
// Hangul, Greek, Cyrillic, symbols) comes from DSperate's own DSi system
// fonts, which are linked in anyway (core/io/dsi_font, tools/make_dsi_font.py).
// The three tables are searched in turn -- normal (Latin, Greek, Cyrillic,
// kana, JIS kanji), China (GB 2312), Korea (Hangul) -- and each Nitro font is
// unpacked the first time a glyph is wanted from it, so a menu that stays in
// ASCII never pays for it.
#pragma once
#include "core/types.h"

namespace ds::sdl::uifont {

// Which of a table's three fonts: 16x21, 12x16, or the small one (10x12 in
// the normal table, 12x13 / 12x12 in the Chinese and Korean ones).
enum class Size : u8 { Large = 0, Medium = 1, Small = 2 };

struct Glyph {
  const u8* tile = nullptr;   // cell_w x cell_h, 2 bpp, rows packed from the MSB
  int cell_w = 0, cell_h = 0;
  int base = 0;               // the baseline row: the first row below the text
  int left = 0, ink = 0, adv = 0;   // CWDH: ink start, ink width, advance
  // 0..3, pixel (x, y) of the cell.
  int px(int x, int y) const {
    const int i = (y * cell_w + x) * 2;
    return (tile[i >> 3] >> (6 - (i & 7))) & 3;
  }
};

// False when no table has `cp` (or the tables did not unpack). Thread-safe:
// the fonts unpack once, under a static initialiser.
bool find(u32 cp, Size size, Glyph& out);

}  // namespace ds::sdl::uifont
