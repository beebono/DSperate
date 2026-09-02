// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's navigation and the drawing's bounds.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"
#include "core/cheat/database.h"
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

// --- the cheats page -------------------------------------------------------

// A small list with a note, an ordinary group and an exclusive one.
struct Fixture {
  std::vector<ds::cheat::Code> codes;
  std::vector<ds::cheat::Group> groups;
  Fixture() {
    groups.push_back({"Misc", "", false});
    groups.push_back({"Difficulty", "", true});
    auto add = [&](const char* name, int group, bool note) {
      ds::cheat::Code c;
      c.name = name;
      c.group = group;
      if (!note) c.words = {0x02000000, 1};
      codes.push_back(c);
    };
    add("(M) note", 0, true);      // 0
    add("Infinite Lives", 0, false);  // 1
    add("Infinite Coins", 0, false);  // 2
    add("Easy", 1, false);            // 3
    add("Normal", 1, false);          // 4
    add("Hard", 1, false);            // 5
  }
};

// Reaching the page: the root row exists only when a database matched.
void open_cheats(Menu& m) {
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));    // SAVE, LOAD, SLOT, CHEATS
  m.input(press(B::BTN_A));
}

void test_cheats_row_hidden_without_codes() {
  Menu m;
  m.set_open(true);
  // With no cheats the root page is five rows and the fourth is RESUME.
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume);
  // Empty is the same as absent.
  std::vector<ds::cheat::Code> none;
  std::vector<ds::cheat::Group> no_groups;
  m.set_cheats(&none, &no_groups);
  m.set_open(true);
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume);
}

// With codes the row appears, and the rows after it shift down by one.
void test_cheats_row_shifts_the_rest() {
  Fixture f;
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  m.set_open(true);
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                        // CHEATS: opens a page, no result
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);   // and B comes back to it
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Resume); // RESUME is now the fifth
}

// A is a toggle, and only codes can be selected -- not headings, not notes.
void test_cheat_toggle() {
  Fixture f;
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_cheats(m);
  CHECK(!m.cheats_dirty());
  // The first selectable line is the first real code, not the heading above
  // it and not the note before it.
  m.input(press(B::BTN_A));
  CHECK(f.codes[1].enabled);
  CHECK(!f.codes[0].enabled);      // the note was never selectable
  CHECK(m.cheats_dirty());
  m.clear_cheats_dirty();
  m.input(press(B::BTN_A));        // and A again turns it off
  CHECK(!f.codes[1].enabled);
  CHECK(m.cheats_dirty());
}

// In a group the database marks as alternatives, only one may be on.
void test_exclusive_group() {
  Fixture f;
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_cheats(m);
  // Down to "Easy" (codes 1, 2, then the Difficulty heading is skipped).
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(f.codes[3].enabled);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));        // "Normal"
  CHECK(f.codes[4].enabled);
  CHECK(!f.codes[3].enabled);      // which turned "Easy" off
  // The ordinary group is unaffected by any of it.
  CHECK(!f.codes[1].enabled && !f.codes[2].enabled);
}

// Turning one off does not turn a sibling on.
void test_exclusive_off_is_not_a_switch() {
  Fixture f;
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_cheats(m);
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));        // Easy on
  m.input(press(B::BTN_A));        // Easy off again
  CHECK(!f.codes[3].enabled && !f.codes[4].enabled && !f.codes[5].enabled);
}

// The selection stops at the ends instead of wrapping: a list of thousands
// is not one to wrap by accident.
void test_cheat_navigation_clamps() {
  Fixture f;
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_cheats(m);
  for (int i = 0; i < 20; ++i) m.input(press(B::BTN_UP));
  m.input(press(B::BTN_A));
  CHECK(f.codes[1].enabled);       // still the first code
  m.clear_cheats_dirty();
  for (int i = 0; i < 50; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  CHECK(f.codes[5].enabled);       // and the last
}

// The page must stay on screen however far down the list it is scrolled.
void test_cheats_draw_bounds() {
  Fixture f;
  // A list long enough to scroll, with names long enough to need truncating.
  for (int i = 0; i < 200; ++i) {
    ds::cheat::Code c;
    c.name = "A very long cheat name that will not fit across the panel " + std::to_string(i);
    c.group = i % 3 == 0 ? 0 : 1;
    c.words = {0x02000000, 1};
    f.codes.push_back(c);
  }
  const u32 w = ds::SCREEN_W, h = ds::SCREEN_H;
  std::vector<u32> fb((w + 2) * (h + 2), 0xDEADBEEF);
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_cheats(m);
  for (int step = 0; step < 210; ++step) {
    std::fill(fb.begin(), fb.end(), 0xDEADBEEF);
    m.draw(ds::sdl::Blit{fb.data() + (w + 2) + 1, w + 2, h, nullptr});
    for (u32 x = 0; x < w + 2; ++x) CHECK(fb[x] == 0xDEADBEEF);
    for (u32 x = 0; x < w + 2; ++x) CHECK(fb[(h + 1) * (w + 2) + x] == 0xDEADBEEF);
    for (u32 y = 0; y < h + 2; ++y) CHECK(fb[y * (w + 2)] == 0xDEADBEEF);
    for (u32 y = 0; y < h + 2; ++y) CHECK(fb[y * (w + 2) + w + 1] == 0xDEADBEEF);
    m.input(press(B::BTN_DOWN));
  }
}

// An empty page draws rather than dividing by zero on the scroll bar.
void test_cheats_draw_empty() {
  std::vector<ds::cheat::Code> none;
  std::vector<ds::cheat::Group> no_groups;
  std::vector<u32> fb(ds::SCREEN_W * ds::SCREEN_H, 0);
  Menu m;
  m.set_cheats(&none, &no_groups);
  m.set_open(true);
  m.draw(ds::sdl::Blit{fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr});
}

// --- key repeat and the scrolling name ------------------------------------

// A flat list, so a step is a step: no headings or notes in the way.
struct FlatFixture {
  std::vector<ds::cheat::Code> codes;
  std::vector<ds::cheat::Group> groups;
  explicit FlatFixture(int n, const char* name = "Cheat") {
    for (int i = 0; i < n; ++i) {
      ds::cheat::Code c;
      c.name = std::string(name) + " " + std::to_string(i);
      c.words = {0x02000000, 1};
      codes.push_back(c);
    }
  }
  // Which code is selected, read off by toggling it.
  int selected(Menu& m) {
    m.input(press(B::BTN_A));
    for (size_t i = 0; i < codes.size(); ++i)
      if (codes[i].enabled) { codes[i].enabled = false; return static_cast<int>(i); }
    return -1;
  }
};

void open_flat(Menu& m) {
  m.set_open(true);
  m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN)); m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
}

// Holding a direction does nothing until the delay has passed, then steps.
// A real press arrives with the button already held, which is what starts
// the timer, so the tests send both.
void test_key_repeat() {
  const u32 down = 1u << B::BTN_DOWN;
  {
    FlatFixture f(40);
    Menu m;
    m.set_cheats(&f.codes, &f.groups);
    open_flat(m);
    m.update(down, down, 0);                             // the press itself: one row
    for (int t = 0; t < 39; ++t) m.update(0, down, 10);  // 390 ms, just under the delay
    CHECK(f.selected(m) == 1);                           // nothing repeated yet
  }
  {
    FlatFixture f(40);
    Menu m;
    m.set_cheats(&f.codes, &f.groups);
    open_flat(m);
    m.update(down, down, 0);
    for (int t = 0; t < 40; ++t) m.update(0, down, 10);  // 400 ms: the first repeat
    CHECK(f.selected(m) == 2);
  }
  {
    FlatFixture f(40);
    Menu m;
    m.set_cheats(&f.codes, &f.groups);
    open_flat(m);
    m.update(down, down, 0);
    for (int t = 0; t < 51; ++t) m.update(0, down, 10);  // 400 + two 55 ms steps
    CHECK(f.selected(m) == 4);
  }
}

// Letting go and pressing again starts the wait over, so a series of taps
// does not accelerate.
void test_key_repeat_restarts() {
  FlatFixture f(40);
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_flat(m);
  const u32 down = 1u << B::BTN_DOWN;
  m.update(down, down, 0);
  for (int t = 0; t < 39; ++t) m.update(0, down, 10);   // just under the delay
  for (int t = 0; t < 5; ++t) m.update(0, 0, 10);       // released
  m.update(down, down, 0);                              // and pressed again
  for (int t = 0; t < 39; ++t) m.update(0, down, 10);
  CHECK(f.selected(m) == 2);                            // two presses, no repeats
}

// The other pages keep their one-step-per-press feel.
void test_key_repeat_only_on_cheats() {
  FlatFixture f(40);
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  m.set_open(true);
  const u32 down = 1u << B::BTN_DOWN;
  for (int t = 0; t < 200; ++t) m.update(0, down, 10);  // two seconds on the root page
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::Save);   // still the first row
}

// A name too long for its row scrolls after a pause; a short one never does.
// The overflow is measured while drawing, so the page has to be drawn first.
void test_marquee() {
  const u32 w = ds::SCREEN_W, h = ds::SCREEN_H;
  std::vector<u32> fb(w * h, 0);
  const ds::sdl::Blit d{fb.data(), w, h, nullptr};

  FlatFixture longnames(4, "An extremely long cheat name that cannot possibly fit across the panel");
  Menu m;
  m.set_cheats(&longnames.codes, &longnames.groups);
  open_flat(m);
  m.draw(d);
  m.clear_dirty();
  // Nothing moves during the initial pause.
  for (int t = 0; t < 40; ++t) { m.update(0, 0, 10); m.draw(d); }
  CHECK(!m.dirty());
  // Then it starts, and keeps asking to be redrawn.
  for (int t = 0; t < 20; ++t) { m.update(0, 0, 10); m.draw(d); }
  CHECK(m.dirty());

  // A short name never scrolls, however long it is selected.
  FlatFixture shortnames(4, "Short");
  Menu m2;
  m2.set_cheats(&shortnames.codes, &shortnames.groups);
  open_flat(m2);
  m2.draw(d);
  m2.clear_dirty();
  for (int t = 0; t < 500; ++t) { m2.update(0, 0, 10); m2.draw(d); }
  CHECK(!m2.dirty());
}

// Moving the selection puts the new name back to its start.
void test_marquee_resets_on_move() {
  const u32 w = ds::SCREEN_W, h = ds::SCREEN_H;
  std::vector<u32> fb(w * h, 0);
  const ds::sdl::Blit d{fb.data(), w, h, nullptr};
  FlatFixture f(10, "An extremely long cheat name that cannot possibly fit across the panel");
  Menu m;
  m.set_cheats(&f.codes, &f.groups);
  open_flat(m);
  m.draw(d);
  // Scroll well into it, then move: the new row starts from still again.
  for (int t = 0; t < 80; ++t) { m.update(0, 0, 10); m.draw(d); }
  m.update(press(B::BTN_DOWN), 0, 10);
  m.draw(d);
  m.clear_dirty();
  for (int t = 0; t < 40; ++t) { m.update(0, 0, 10); m.draw(d); }
  CHECK(!m.dirty());               // back inside the initial pause
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
  test_cheats_row_hidden_without_codes();
  test_cheats_row_shifts_the_rest();
  test_cheat_toggle();
  test_exclusive_group();
  test_exclusive_off_is_not_a_switch();
  test_cheat_navigation_clamps();
  test_cheats_draw_bounds();
  test_cheats_draw_empty();
  test_key_repeat();
  test_key_repeat_restarts();
  test_key_repeat_only_on_cheats();
  test_marquee();
  test_marquee_resets_on_move();
  std::printf("menu: ok\n");
  return 0;
}
