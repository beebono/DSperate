// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's navigation and the drawing's bounds.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"
#include "check.h"

#include <algorithm>
#include <vector>

using ds::u32;
using ds::sdl::Menu;
using B = ds::io::Io::Button;

namespace {
u32 press(B b) { return 1u << b; }

// Every row of the root page, in order, so the switch and the labels cannot
// drift apart silently.
void test_root_rows() {
  Menu m;
  m.set_open(true);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);          // row 0
  CHECK(m.input(press(B::BTN_DOWN)) == Menu::Result::None);
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Load);          // row 1
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);          // row 2 opens the slot page
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);          // and B comes back to it
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume);        // row 3
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Quit);          // row 4
}

void test_wrap_and_back() {
  Menu m;
  m.set_open(true);
  m.input(press(B::BTN_UP));                                      // wraps to the last row
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Quit);
  m.set_open(true);
  for (int i = 0; i < 5; ++i) m.input(press(B::BTN_DOWN));        // a full cycle
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);
  // B on the root page leaves the menu; on the slot page it only goes back.
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::Resume);
}

void test_slot_selection() {
  Menu m;
  m.set_open(true);
  m.set_slot(4);
  // Left/right on the slot row adjust it without opening the page, and wrap.
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(m.slot() == 5);
  for (int i = 0; i < 6; ++i) m.input(press(B::BTN_LEFT));
  CHECK(m.slot() == 9);                                           // 5 -> 0 -> wraps to 9
  // The page opens on the current slot, laid out as two columns of five:
  // slots 0-4 on the left, 5-9 on the right.
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_DOWN));                                    // 9 is the foot of its column
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  CHECK(m.slot() == 5);                                           // wrapped to the column's head
  // Back on the root page, the cursor is on the slot row it came from.
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);          // opens the page again
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);          // back to root, not a resume
  CHECK(m.slot() == 5);                                           // B does not commit a move
}

// Up/down stay inside a column; left and right are what crosses between them.
void test_slot_grid_navigation() {
  Menu m;
  m.set_open(true);
  m.set_slot(0);
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                       // the slot page, on 0
  m.input(press(B::BTN_UP));                                      // head of the left column wraps to its foot
  m.input(press(B::BTN_A));
  CHECK(m.slot() == 4);
  m.input(press(B::BTN_A));                                       // reopen on 4
  m.input(press(B::BTN_RIGHT));                                   // across to the right column
  m.input(press(B::BTN_A));
  CHECK(m.slot() == 9);
  m.input(press(B::BTN_A));                                       // reopen on 9
  m.input(press(B::BTN_LEFT));                                    // and back again
  m.input(press(B::BTN_A));
  CHECK(m.slot() == 4);
}

// Slot rows only exist on the slot page: left/right elsewhere must not move it.
void test_slot_untouched_off_row() {
  Menu m;
  m.set_open(true);
  m.set_slot(3);
  m.input(press(B::BTN_RIGHT));                                   // row 0
  CHECK(m.slot() == 3);
}

// The panel must stay inside the DS screen on both pages, whatever the row.
void test_draw_bounds() {
  const u32 w = ds::SCREEN_W, h = ds::SCREEN_H;
  std::vector<u32> fb((w + 2) * (h + 2), 0xDEADBEEF);
  Menu m;
  m.set_open(true);
  for (int page = 0; page < 2; ++page) {
    for (int row = 0; row < 10; ++row) {
      std::fill(fb.begin(), fb.end(), 0xDEADBEEF);
      // Draw into the middle of a larger buffer: a guard row and column on
      // every side catches a write past the screen.
      m.draw(ds::sdl::Blit{fb.data() + (w + 2) + 1, w + 2, h, nullptr});
      for (u32 x = 0; x < w + 2; ++x) CHECK(fb[x] == 0xDEADBEEF);                        // above
      for (u32 x = 0; x < w + 2; ++x) CHECK(fb[(h + 1) * (w + 2) + x] == 0xDEADBEEF);    // below
      for (u32 y = 0; y < h + 2; ++y) CHECK(fb[y * (w + 2)] == 0xDEADBEEF);              // left
      for (u32 y = 0; y < h + 2; ++y) CHECK(fb[y * (w + 2) + w + 1] == 0xDEADBEEF);      // right
      m.input(press(B::BTN_DOWN));
    }
    if (page == 0) { m.set_open(true); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_A)); }
  }
}

void test_text_metrics() {
  std::vector<u32> fb(ds::SCREEN_W * ds::SCREEN_H, 0);
  const ds::sdl::Blit d{fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr};
  CHECK(ds::sdl::text_width(2, "") == 0);
  CHECK(ds::sdl::text_width(1, "A") == 5);
  CHECK(ds::sdl::text_width(2, "AB") == 22);       // 10 + gap 2 + 10
  CHECK(ds::sdl::draw_text(d, 10, 10, 2, 0xFFFFFFFF, "AB") == 10 + 22);
  // Lowercase folds to uppercase, so the two draw the same pixels.
  std::vector<u32> a(ds::SCREEN_W * ds::SCREEN_H, 0), b = a;
  ds::sdl::draw_text(ds::sdl::Blit{a.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, 4, 4, 2, 0xFFFFFFFF, "save");
  ds::sdl::draw_text(ds::sdl::Blit{b.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, 4, 4, 2, 0xFFFFFFFF, "SAVE");
  CHECK(a == b);
}

void test_dim() {
  std::vector<u32> px = {0xFFFFFFFF, 0xFF000000, 0xFF804020};
  ds::sdl::dim_framebuffer(px.data(), static_cast<u32>(px.size()));
  CHECK(px[0] == 0xFF7F7F7F);
  CHECK(px[1] == 0xFF000000);
  CHECK(px[2] == 0xFF402010);   // alpha kept, every channel halved
}

} // namespace

int main() {
  test_root_rows();
  test_wrap_and_back();
  test_slot_selection();
  test_slot_grid_navigation();
  test_slot_untouched_off_row();
  test_draw_bounds();
  test_text_metrics();
  test_dim();
  std::printf("menu: ok\n");
  return 0;
}
