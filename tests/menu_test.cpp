// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's navigation and the drawing's bounds.
#include "frontend/sdl/menu.h"
#include "core/io/io.h"
#include "core/cheat/database.h"
#include "frontend/sdl/settings.h"
#include "check.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
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
      m.draw(ds::sdl::Canvas{fb.data() + (w + 2) + 1, static_cast<ds::u32>(w + 2), w, h});
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
  const ds::sdl::Canvas d{fb.data(), ds::SCREEN_W, ds::SCREEN_W, ds::SCREEN_H};
  CHECK(ds::sdl::text_width(2, "") == 0);
  CHECK(ds::sdl::text_width(1, "A") == 5);
  CHECK(ds::sdl::text_width(2, "AB") == 22);       // 10 + gap 2 + 10
  CHECK(ds::sdl::draw_text(d, 10, 10, 2, 0xFFFFFFFF, "AB") == 10 + 22);
  // Lowercase folds to uppercase, so the two draw the same pixels.
  std::vector<u32> a(ds::SCREEN_W * ds::SCREEN_H, 0), b = a;
  ds::sdl::draw_text(ds::sdl::Canvas{a.data(), ds::SCREEN_W, ds::SCREEN_W, ds::SCREEN_H}, 4, 4, 2, 0xFFFFFFFF, "save");
  ds::sdl::draw_text(ds::sdl::Canvas{b.data(), ds::SCREEN_W, ds::SCREEN_W, ds::SCREEN_H}, 4, 4, 2, 0xFFFFFFFF, "SAVE");
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
    m.draw(ds::sdl::Canvas{fb.data() + (w + 2) + 1, static_cast<ds::u32>(w + 2), w, h});
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
  m.draw(ds::sdl::Canvas{fb.data(), ds::SCREEN_W, ds::SCREEN_W, ds::SCREEN_H});
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
  const ds::sdl::Canvas d{fb.data(), static_cast<ds::u32>(w), w, h};

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
  const ds::sdl::Canvas d{fb.data(), static_cast<ds::u32>(w), w, h};
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


// A settings host with nothing behind it: the values live in a map, and the
// dependency and tier questions are answered by fields the test sets. Enough
// for the menu's own behaviour, which is what these tests are about.
struct FakeHost : ds::sdl::SettingsHost {
  std::map<std::string, std::string> kv;
  std::map<std::string, bool> off;      // rows the test has switched off
  std::vector<std::string> writes;      // keys set, in order
  bool per_game = false, game = true, pad = false, capturing_ = false;
  const char* user_note = nullptr;   // non-null when a firmware dump holds them
  std::string pending;                  // what the "device" is about to report
  int commits = 0;

  std::string get(const char* key) const override {
    const auto it = kv.find(key);
    return it == kv.end() ? "" : it->second;
  }
  void set(const char* key, const std::string& v) override { kv[key] = v; writes.push_back(key); }
  bool enabled(const ds::sdl::Setting& s) const override {
    const auto it = off.find(s.key);
    return it == off.end() || !it->second;
  }
  const char* disabled_reason(const ds::sdl::Setting& s) const override { return enabled(s) ? "" : "NO"; }
  bool value_allowed(const ds::sdl::Setting&, const char*) const override { return true; }
  void commit() override { ++commits; }
  bool save_per_game() const override { return per_game && game; }
  void set_save_per_game(bool on) override { per_game = on && game; }
  bool has_game() const override { return game; }

  int binding_count(bool) const override { return 4; }
  Binding binding(bool p, int i) const override {
    Binding b;
    b.key = (p ? "pad.k" : "keys.k") + std::to_string(i);
    b.label = "ROW" + std::to_string(i);
    const auto it = kv.find(b.key);
    b.value = it == kv.end() ? "NONE" : it->second;
    return b;
  }
  bool has_pad() const override { return pad; }
  void begin_capture(bool) override { capturing_ = true; }
  void cancel_capture() override { capturing_ = false; }
  bool capturing() const override { return capturing_; }
  std::string take_capture() override {
    if (pending.empty()) return "";
    std::string out; out.swap(pending); capturing_ = false; return out;
  }
  void bind(const std::string& k, const std::string& v) override { kv[k] = v; writes.push_back(k); }
  void reset_bindings(bool) override { writes.push_back("reset"); }
  std::vector<std::string> collisions() const override { return {}; }
  const char* user_settings_note() const override { return user_note; }
};

// Walking into a page and back out again lands where it left, at every depth.
// The old flat page state shared one row between the root and the slot page
// and had to reset it on the way in and out, which a stack cannot do.
void test_page_stack() {
  FakeHost h;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  // SAVE, LOAD, SLOT, OPTIONS, RESUME, QUIT -- no cheats row without codes.
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);      // into Options
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);      // into Emulation
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);      // back to Options
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);      // back to the root
  // ... on the row that opened it, so A goes straight back in.
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);
  // And B at the root resumes rather than popping past it.
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::Resume);
}

// The slot page's selection is its own: entering it and coming back must not
// move the root page's row, and vice versa.
void test_slot_row_is_separate() {
  Menu m;
  m.set_open(true);
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));                                 // the slot row
  m.input(press(B::BTN_A));                                    // into the slot page
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_DOWN));                                 // move within it
  m.input(press(B::BTN_B));                                    // back out without choosing
  // Still on the slot row: A opens the slot page again rather than doing
  // whatever the row two below happens to be.
  CHECK(m.input(press(B::BTN_A)) == Menu::Result::None);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::None);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::Resume);
}

// Stepping a value: the ends clamp, a sentinel sits one step below the range,
// and a percent survives being shown and written back.
void test_setting_steps() {
  FakeHost h;
  const ds::sdl::Setting* t = ds::sdl::kEmuSettings;
  const int n = ds::sdl::settings_count(t);
  CHECK(n > 0);
  const auto find = [&](const char* key) -> const ds::sdl::Setting& {
    for (int i = 0; i < n; ++i) if (!std::strcmp(t[i].key, key)) return t[i];
    CHECK(false);
    return t[0];
  };
  const ds::sdl::Setting& fs = find("emu.frameskip");
  CHECK(ds::sdl::step_value(fs, "0", -1, h) == "0");            // clamped at the bottom
  CHECK(ds::sdl::step_value(fs, "3", +1, h) == "3");            // and at the top
  CHECK(ds::sdl::step_value(fs, "1", +1, h) == "2");
  // A sentinel: down off the bottom of the range, and back up onto it. The
  // range starts at 2, since 1X is real time and that is not fast forwarding.
  const ds::sdl::Setting& ff = find("emu.ff_speed");
  CHECK(ds::sdl::step_value(ff, "2", -1, h) == "0");
  CHECK(ds::sdl::display_value(ff, "0") == "UNLIMITED");
  CHECK(ds::sdl::step_value(ff, "0", -1, h) == "0");            // nothing below it
  CHECK(ds::sdl::step_value(ff, "0", +1, h) == "2");
  // A number says what it counts.
  CHECK(ds::sdl::display_value(ff, "4") == "4X");
  // A boolean wraps, because a two-entry list has to.
  const ds::sdl::Setting& oc = find("emu.cpu_oc");
  CHECK(ds::sdl::step_value(oc, "false", +1, h) == "true");
  CHECK(ds::sdl::step_value(oc, "true", +1, h) == "false");
}

// A percent row keeps a 0..1 double in the file: stepping it must not creep,
// or a value would drift every time the row was moved.
void test_percent_round_trip() {
  FakeHost h;
  const ds::sdl::Setting* t = ds::sdl::kLayoutSettings;
  const ds::sdl::Setting* pip = nullptr;
  for (int i = 0; i < ds::sdl::settings_count(t); ++i)
    if (!std::strcmp(t[i].key, "video.pip_alpha")) pip = &t[i];
  CHECK(pip != nullptr);
  std::string v = "1";
  CHECK(ds::sdl::display_value(*pip, v) == "100%");
  // All the way down and back up again lands on the same string it started.
  for (int i = 0; i < 10; ++i) v = ds::sdl::step_value(*pip, v, -1, h);
  CHECK(ds::sdl::display_value(*pip, v) == "0%");
  for (int i = 0; i < 10; ++i) v = ds::sdl::step_value(*pip, v, +1, h);
  CHECK(ds::sdl::display_value(*pip, v) == "100%");
  CHECK(ds::sdl::step_value(*pip, v, +1, h) == v);   // clamped
}

// A value the file already holds outside the menu's range is shown as it
// stands and stepped from where it is, not clamped the moment the page opens.
void test_out_of_range_value_is_kept() {
  FakeHost h;
  const ds::sdl::Setting* t = ds::sdl::kEmuSettings;
  const ds::sdl::Setting* fs = nullptr;
  for (int i = 0; i < ds::sdl::settings_count(t); ++i)
    if (!std::strcmp(t[i].key, "emu.frameskip")) fs = &t[i];
  CHECK(fs != nullptr);
  CHECK(ds::sdl::display_value(*fs, "8") == "8");      // shown as the file wrote it
  // Touching it brings it into the menu's range, from either direction: the
  // page offers 0..3, so once the player moves the row that is what it holds.
  CHECK(ds::sdl::step_value(*fs, "8", -1, h) == "3");
  CHECK(ds::sdl::step_value(*fs, "8", +1, h) == "3");
  // A choice the table does not know reads as itself rather than being
  // silently redrawn as something else.
  const ds::sdl::Setting* ch = nullptr;
  for (int i = 0; i < ds::sdl::settings_count(ds::sdl::kVideoSettings); ++i)
    if (!std::strcmp(ds::sdl::kVideoSettings[i].key, "video.chunky")) ch = &ds::sdl::kVideoSettings[i];
  CHECK(ch != nullptr);
  CHECK(ds::sdl::display_value(*ch, "wibble") == "wibble");
}

// Every table's stated default has to be one the table itself can represent,
// or the first press on that row would jump somewhere unrelated.
void test_defaults_are_reachable() {
  FakeHost h;
  for (const ds::sdl::Setting* t : {ds::sdl::kEmuSettings, ds::sdl::kVideoSettings,
                                    ds::sdl::kLayoutSettings, ds::sdl::kUserSettings}) {
    for (int i = 0; i < ds::sdl::settings_count(t); ++i) {
      const ds::sdl::Setting& s = t[i];
      const std::string def = ds::sdl::default_value(s);
      // Shown as something, and stepping from it stays inside the range.
      CHECK(!ds::sdl::display_value(s, def).empty());
      if (s.type == ds::sdl::Setting::Type::Text) continue;
      const std::string up = ds::sdl::step_value(s, def, +1, h);
      CHECK(!ds::sdl::display_value(s, up).empty());
      CHECK(!ds::sdl::display_value(s, ds::sdl::step_value(s, up, -1, h)).empty());
    }
  }
}

// The row walk steps over rows the host has switched off, in both directions,
// and a page whose last rows are all off must not strand the selection.
void test_disabled_rows_are_skipped() {
  FakeHost h;
  // Everything but the first and the last row of the emulation page is off.
  const ds::sdl::Setting* t = ds::sdl::kEmuSettings;
  const int n = ds::sdl::settings_count(t);
  for (int i = 1; i < n - 1; ++i) h.off[t[i].key] = true;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                    // Options
  m.input(press(B::BTN_A));                                    // Emulation
  // Down lands on the last row, skipping every disabled one between.
  m.input(press(B::BTN_DOWN));
  const size_t before = h.writes.size();
  m.input(press(B::BTN_RIGHT));                                // steps the row it is on
  CHECK(h.writes.size() == before + 1);
  CHECK(h.writes.back() == t[n - 1].key);
  // Down again has nowhere to go and must leave the selection alone.
  m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.back() == t[n - 1].key);
  // And back up to the first, over the same gap.
  m.input(press(B::BTN_UP));
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.back() == t[0].key);
  m.input(press(B::BTN_UP));                                   // nothing above it
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.back() == t[0].key);
}

// A disabled row is never stepped, however it came to be selected.
void test_disabled_row_does_not_step() {
  FakeHost h;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_A));                                    // Emulation, on row 0
  h.off[ds::sdl::kEmuSettings[0].key] = true;                  // switched off underneath it
  m.input(press(B::BTN_RIGHT));
  CHECK(h.writes.empty());
  m.input(press(B::BTN_A));
  CHECK(h.writes.empty());
}

// Anything that would move the picture about waits until the menu closes, so
// the screens do not jump under a page the player is still reading. Every way
// out goes through set_open(false), which is where it happens.
void test_commit_on_closing() {
  FakeHost h;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_A));
  const int at_entry = h.commits;
  m.input(press(B::BTN_RIGHT));
  CHECK(h.commits == at_entry);        // changed something: still nothing done
  m.input(press(B::BTN_DOWN));
  CHECK(h.commits == at_entry);        // left the row: still nothing
  m.input(press(B::BTN_B));
  CHECK(h.commits == at_entry);        // left the page: still nothing
  m.input(press(B::BTN_B));            // back at the root
  CHECK(h.commits == at_entry);
  CHECK(m.input(press(B::BTN_B)) == Menu::Result::Resume);
  m.set_open(false);
  CHECK(h.commits == at_entry + 1);    // ... and now, once
  // Closing again does not do it twice.
  m.set_open(false);
  CHECK(h.commits == at_entry + 1);
}

// The Controls page binds what the device reports, to the row that asked.
void test_controls_binding() {
  FakeHost h;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                    // Options
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));     // CONTROLS
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_DOWN));                                 // row 1
  m.input(press(B::BTN_A));                                    // listen
  CHECK(h.capturing());
  // Nothing is bound until the device reports something.
  m.update(0, 0, 10);
  CHECK(h.writes.empty());
  h.pending = "J";
  m.update(0, 0, 10);
  CHECK(!h.writes.empty());
  CHECK(h.writes.back() == "keys.k1");
  CHECK(h.kv["keys.k1"] == "J");
  CHECK(!h.capturing());
  // Y clears the row it is on; X puts the column back.
  m.input(press(B::BTN_Y));
  CHECK(h.kv["keys.k1"] == "none");
  m.input(press(B::BTN_X));
  CHECK(h.writes.back() == "reset");
}

// The Controls page opens on the pad when there is one: on a handheld that is
// the only input, and the keyboard column would be a dead end.
void test_controls_opens_on_the_pad() {
  for (const bool pad : {false, true}) {
    FakeHost h;
    h.pad = pad;
    Menu m;
    m.set_settings_host(&h);
    m.set_open(true);
    for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
    m.input(press(B::BTN_A));
    for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
    m.input(press(B::BTN_A));
    m.input(press(B::BTN_A));                                  // listen on row 0
    h.pending = "Q";
    m.update(0, 0, 10);
    CHECK(h.writes.back() == (pad ? "pad.k0" : "keys.k0"));
  }
}

// The character editor: the cursor walks the field, the tables wrap, and
// nothing is written until A.
void test_text_editor() {
  FakeHost h;
  h.kv["user.nickname"] = "AB";
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                    // Options
  for (int i = 0; i < 4; ++i) m.input(press(B::BTN_DOWN));     // DS OPTIONS
  m.input(press(B::BTN_A));
  m.input(press(B::BTN_A));                                    // the nickname editor
  CHECK(h.writes.empty());                                     // opening writes nothing
  m.input(press(B::BTN_DOWN));                                 // A -> B
  m.input(press(B::BTN_B));                                    // abandon
  CHECK(h.kv["user.nickname"] == "AB");                        // ... and nothing was kept
  m.input(press(B::BTN_A));                                    // open it again
  m.input(press(B::BTN_DOWN));                                 // A -> B
  m.input(press(B::BTN_A));                                    // commit
  CHECK(h.kv["user.nickname"] == "BB");
  // The cursor moves along the field and past its end, up to the length the
  // firmware keeps; the table wraps rather than running off its end.
  m.input(press(B::BTN_A));
  for (int i = 0; i < 40; ++i) m.input(press(B::BTN_RIGHT));
  for (int i = 0; i < 40; ++i) m.input(press(B::BTN_UP));
  for (int i = 0; i < 8; ++i) m.input(press(B::BTN_L));        // round every table and back
  m.input(press(B::BTN_A));
  CHECK(h.kv["user.nickname"].size() <= 10);                   // never past the field
}

// The DS Options page is editable either way: with a generated firmware the
// values are [user] in the config, with a dump they are the dump's own pages.
// Only where they are kept differs, and the page says which.
void test_ds_options_with_a_firmware_dump() {
  for (const char* note : {static_cast<const char*>(nullptr), "KEPT BESIDE THE FIRMWARE"}) {
    FakeHost h;
    h.user_note = note;
    Menu m;
    m.set_settings_host(&h);
    m.set_open(true);
    for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
    m.input(press(B::BTN_A));
    for (int i = 0; i < 4; ++i) m.input(press(B::BTN_DOWN));
    m.input(press(B::BTN_A));                                  // DS OPTIONS
    m.input(press(B::BTN_DOWN));
    m.input(press(B::BTN_DOWN));                               // FAVOURITE COLOUR, a stepped row
    m.input(press(B::BTN_RIGHT));
    CHECK(!h.writes.empty());
    CHECK(h.writes.back() == "user.colour");
    std::vector<u32> fb(ds::SCREEN_W * ds::SCREEN_H);
    m.draw(ds::sdl::Canvas{fb.data(), ds::SCREEN_W, ds::SCREEN_W, ds::SCREEN_H});
  }
}

// The save-to switch only exists when there is a game to save to.
void test_save_target_switch() {
  FakeHost h;
  Menu m;
  m.set_settings_host(&h);
  m.set_open(true);
  for (int i = 0; i < 3; ++i) m.input(press(B::BTN_DOWN));
  m.input(press(B::BTN_A));                                    // Options
  for (int i = 0; i < 5; ++i) m.input(press(B::BTN_DOWN));     // the save row
  CHECK(!h.save_per_game());
  m.input(press(B::BTN_RIGHT));
  CHECK(h.save_per_game());
  m.input(press(B::BTN_LEFT));
  CHECK(!h.save_per_game());
  // With no game the row is not there, so five downs wrap to a page row and
  // A opens a page rather than toggling anything.
  FakeHost h2;
  h2.game = false;
  Menu m2;
  m2.set_settings_host(&h2);
  m2.set_open(true);
  for (int i = 0; i < 3; ++i) m2.input(press(B::BTN_DOWN));
  m2.input(press(B::BTN_A));
  for (int i = 0; i < 5; ++i) m2.input(press(B::BTN_DOWN));
  m2.input(press(B::BTN_A));
  CHECK(!h2.save_per_game());
}

// Every page, at canvas sizes from a DS screen to a desktop and in portrait,
// stays inside the canvas and writes nothing past its edges.
void test_canvas_sizes() {
  static const int dims[][2] = {{256, 192}, {320, 240}, {480, 272}, {512, 384},
                                {640, 480}, {720, 720}, {1280, 720}, {1920, 1080},
                                {240, 320}, {160, 128}};
  std::vector<ds::cheat::Code> codes;
  std::vector<ds::cheat::Group> groups;
  { ds::cheat::Group g; g.name = "GROUP"; groups.push_back(g); }
  for (int i = 0; i < 50; ++i) {
    ds::cheat::Code c;
    c.name = "A CHEAT WITH A REASONABLY LONG NAME " + std::to_string(i);
    c.group = 0;
    c.words.push_back(1);
    codes.push_back(c);
  }
  std::vector<Menu::GameEntry> games;
  for (int i = 0; i < 30; ++i) games.push_back({"GAME " + std::to_string(i), "/x"});

  for (const auto& wh : dims) {
    const int w = wh[0], h = wh[1];
    // A guard row and column on every side catches a write past the canvas.
    std::vector<u32> fb(static_cast<size_t>(w + 2) * (h + 2), 0xDEADBEEF);
    const ds::sdl::Canvas d{fb.data() + (w + 2) + 1, static_cast<u32>(w + 2), w, h};
    CHECK(ds::sdl::ui_scale(d) >= 2);
    const auto guards_intact = [&] {
      for (int x = 0; x < w + 2; ++x) {
        CHECK(fb[static_cast<size_t>(x)] == 0xDEADBEEF);
        CHECK(fb[static_cast<size_t>(h + 1) * (w + 2) + x] == 0xDEADBEEF);
      }
      for (int y = 0; y < h + 2; ++y) {
        CHECK(fb[static_cast<size_t>(y) * (w + 2)] == 0xDEADBEEF);
        CHECK(fb[static_cast<size_t>(y) * (w + 2) + w + 1] == 0xDEADBEEF);
      }
    };
    FakeHost host;
    Menu m;
    m.set_settings_host(&host);
    m.set_cheats(&codes, &groups);
    m.set_games(&games);
    m.set_open(true);
    // The root, then every page reachable from it.
    m.draw(d); guards_intact();
    for (int i = 0; i < 3; ++i) { m.input(press(B::BTN_DOWN)); m.draw(d); guards_intact(); }
    m.input(press(B::BTN_A));  m.draw(d); guards_intact();     // Options
    for (int page = 0; page < 5; ++page) {
      Menu p;
      p.set_settings_host(&host);
      p.set_cheats(&codes, &groups);
      p.set_open(true);
      for (int i = 0; i < 4; ++i) p.input(press(B::BTN_DOWN)); // cheats row is shown now
      p.input(press(B::BTN_A));                                 // Options
      for (int i = 0; i < page; ++i) p.input(press(B::BTN_DOWN));
      p.input(press(B::BTN_A));
      p.draw(d); guards_intact();
      p.input(press(B::BTN_DOWN)); p.draw(d); guards_intact();
      p.input(press(B::BTN_A));    p.draw(d); guards_intact();  // and whatever A opens there
    }
    // The slot page, the cheats page, the game picker and the notice.
    Menu s;
    s.set_open(true);
    s.input(press(B::BTN_DOWN)); s.input(press(B::BTN_DOWN)); s.input(press(B::BTN_A));
    s.draw(d); guards_intact();
    Menu g;
    g.set_games(&games);
    g.open_games();
    g.draw(d); guards_intact();
    ds::sdl::draw_notice(d, "A GAME WITH A VERY LONG TITLE INDEED", "UNPACKING...", "FIRST LAUNCH ONLY");
    guards_intact();
  }
}

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
  test_page_stack();
  test_slot_row_is_separate();
  test_setting_steps();
  test_percent_round_trip();
  test_out_of_range_value_is_kept();
  test_defaults_are_reachable();
  test_disabled_rows_are_skipped();
  test_disabled_row_does_not_step();
  test_commit_on_closing();
  test_controls_binding();
  test_controls_opens_on_the_pad();
  test_text_editor();
  test_ds_options_with_a_firmware_dump();
  test_save_target_switch();
  test_canvas_sizes();
  std::printf("menu: ok\n");
  return 0;
}
