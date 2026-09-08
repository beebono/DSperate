// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {
namespace {

using T = Setting::Type;

const Choice kOnOff[]   = {{"false", "OFF"}, {"true", "ON"}};
const Choice kSkipMode[] = {{"adaptive", "ADAPTIVE"}, {"fixed", "FIXED"}};
const Choice kIntScale[] = {{"off", "OFF"}, {"under", "UNDER"}, {"over", "OVER"}};
const Choice kSeam[]     = {{"dark", "DARK"}, {"blend", "BLEND"}, {"blend_linear", "BLEND LINEAR"}};
// The file's own words on the left. "mean" is the ordinary cell and reads as
// DEFAULT; the rest are named for what they do rather than how they do it.
const Choice kChunky[]   = {{"false", "OFF"}, {"mean", "DEFAULT"}, {"extreme", "ADAPTIVE"},
                            {"mode", "COMMON"}, {"tl", "FIRST"}, {"min", "DARKEST"}, {"max", "LIGHTEST"}};
const Choice kScreen[]   = {{"top", "TOP"}, {"bottom", "BOTTOM"}};
const Choice kCorner[]   = {{"tl", "TOP LEFT"}, {"tr", "TOP RIGHT"}, {"bl", "BOTTOM LEFT"}, {"br", "BOTTOM RIGHT"}};

// Shorthand for the common shapes, so a table row reads as its own contents.
constexpr Setting boolean(const char* k, const char* l, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Bool, kOnOff, 2, 0, 0, 0, nullptr, nullptr, def, f, d, n};
}
constexpr Setting pick(const char* k, const char* l, const Choice* c, u8 nc, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Pick, c, nc, 0, 0, 0, nullptr, nullptr, def, f, d, n};
}
constexpr Setting number(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                         const char* sv = nullptr, const char* sl = nullptr) {
  return Setting{k, l, T::Int, nullptr, 0, lo, hi, st, sv, sl, def, f, d, n};
}
constexpr Setting percent(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                          const char* sv = nullptr, const char* sl = nullptr) {
  return Setting{k, l, T::Percent, nullptr, 0, lo, hi, st, sv, sl, def, f, d, n};
}
constexpr Setting end() { return Setting{nullptr, nullptr, T::Bool, nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr, 0, Dep::None, nullptr}; }

} // namespace

const Setting kEmuSettings[] = {
  number("emu.frameskip", "FRAMESKIP", 0, 3, 1, "0", FlagLive, Dep::None,
         "DRAW FEWER FRAMES. THE GAME STILL RUNS IN FULL"),
  pick("emu.frameskip_mode", "FRAMESKIP MODE", kSkipMode, 2, "adaptive", FlagLive, Dep::FrameskipMode,
       "ADAPTIVE SKIPS ONLY WHILE BEHIND REAL TIME"),
  boolean("emu.cpu_oc", "CPU OC", "false", FlagLive | FlagInexact, Dep::None,
          "FASTER. TIMER-RACE GAMES DRIFT. TURN OFF IF A GAME MISBEHAVES"),
  boolean("emu.timing_oc", "TIMING OC", "false", FlagLive | FlagInexact, Dep::None,
          "FASTEST AND LEAST SAFE. GAMES THAT PACE ON THE 3D FIFO WILL BREAK"),
  boolean("emu.fast_load", "FAST LOAD", "false", FlagLive | FlagInexact, Dep::None,
          "SHORTER LOADING SCREENS. GAMES THAT RACE THE CARD CAN MISBEHAVE"),
  number("emu.ff_speed", "FAST FORWARD SPEED", 1, 16, 1, "0", FlagLive, Dep::None,
         "HOW FAST THE FAST FORWARD HOTKEY RUNS", "0", "UNLIMITED"),
  number("emu.ff_skip", "FAST FORWARD SKIP", 0, 9, 1, "3", FlagLive, Dep::None,
         "WHILE FAST FORWARDING, SHOW ONE FRAME IN THIS MANY PLUS ONE"),
  boolean("emu.autosave", "AUTOSAVE ON QUIT", "false", FlagLive, Dep::None,
          "SAVE A STATE WHEN THE EMULATOR EXITS, TO RESUME FROM"),
  end(),
};

const Setting kVideoSettings[] = {
  pick("video.integer_scale", "INTEGER SCALE", kIntScale, 3, "off", FlagReopen, Dep::None,
       "WHOLE PANEL PIXELS PER DS PIXEL. UNDER LETTERBOXES, OVER CROPS"),
  boolean("video.linear", "BILINEAR", "false", FlagReopen, Dep::PanelEffects,
          "SMOOTH SCALING. OVERRIDES THE GRID, SEAMS AND CHUNKY"),
  percent("video.lcd_grid", "LCD GRID", 0, 100, 10, "0", FlagReopen, Dep::GridSeam,
          "A DARK SEAM AROUND EVERY DS PIXEL, LIKE THE ORIGINAL SCREEN"),
  pick("video.seam", "SEAM", kSeam, 3, "dark", FlagReopen, Dep::GridSeam,
       "DARK DRAWS THE GRID. BLEND SOFTENS ONLY THE STRADDLING PIXEL"),
  pick("video.chunky", "CHUNKY", kChunky, 7, "false", FlagReopen, Dep::Chunky,
       "DRAW BLOCKS OF DS PIXELS AS ONE FLAT CELL, FOR PANELS AT ODD SCALES"),
  number("video.chunky_cell", "CHUNKY CELL", 2, 8, 1, "auto", FlagReopen, Dep::ChunkyCell,
         "PANEL PIXELS PER CELL", "auto", "AUTO"),
  boolean("video.aa", "ANTI-ALIASING", "false", FlagLive, Dep::None,
          "SMOOTH 3D EDGES AS THE HARDWARE DID. OFF IS CHEAPER"),
  boolean("video.fps", "FPS COUNTER", "false", FlagLive, Dep::None,
          "FRAMES PER SECOND IN THE CORNER OF THE SCREEN"),
  boolean("video.fullscreen", "FULLSCREEN", "false", FlagLive, Dep::Windowed, nullptr),
  end(),
};

const Setting kLayoutSettings[] = {
  pick("video.screen", "MAIN SCREEN", kScreen, 2, "top", FlagLive, Dep::None,
       "THE SCREEN SHOWN ALONE, LARGE OR FIRST"),
  pick("video.pip_corner", "PIP CORNER", kCorner, 4, "br", FlagLive, Dep::Pip,
       "WHERE THE SMALL SCREEN SITS"),
  percent("video.pip_scale", "PIP SIZE", 10, 90, 5, "0.33", FlagLive, Dep::Pip,
          "HOW BIG THE SMALL SCREEN IS AGAINST THE LARGE ONE"),
  percent("video.pip_alpha", "PIP OPACITY", 0, 100, 10, "1", FlagLive, Dep::Pip,
          "HOW SOLID THE SMALL SCREEN IS AT REST"),
  number("video.pip_touch_hold", "PIP TOUCH HOLD", 10, 300, 10, "60", FlagLive, Dep::PipTouchHold,
         "FRAMES THE SMALL SCREEN STAYS SOLID AFTER IT IS TOUCHED", "0", "NEVER FADE"),
  percent("video.dominant_ratio", "DOMINANT RATIO", 10, 90, 5, "auto", FlagLive, Dep::Dominant,
          "THE SMALLER SCREEN'S SIZE. AUTO FITS WHOLE PIXELS", "auto", "AUTO"),
  percent("video.dominant_threshold", "DOMINANT THRESHOLD", 10, 99, 5, "0.25", FlagLive, Dep::DominantThreshold,
          "THE SMALLEST SECONDARY AUTO WILL ACCEPT"),
  end(),
};

int settings_count(const Setting* table) {
  int n = 0;
  while (table[n].key) ++n;
  return n;
}

namespace {

bool truthy(const std::string& v) {
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

// A percent row keeps a 0..1 double in the file. Rounded to the nearest whole
// percent so that stepping and displaying agree: a value that showed 50 must
// come back as 50 after a write, or a row would creep every time it was moved.
int percent_of(const std::string& v) {
  return static_cast<int>(std::lround(std::atof(v.c_str()) * 100.0));
}
std::string percent_str(int p) {
  char buf[16];
  // Two decimals is enough for whole percents and keeps the file readable.
  std::snprintf(buf, sizeof buf, "%.2f", p / 100.0);
  // Trim the trailing zeros a round number leaves behind ("0.50" -> "0.5").
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

int choice_index(const Setting& s, const std::string& v) {
  for (int i = 0; i < s.nchoices; ++i) {
    if (v == s.choices[i].value) return i;
    // Booleans are written half a dozen ways in a hand-edited file.
    if (s.type == T::Bool && truthy(v) == truthy(s.choices[i].value)) return i;
  }
  return -1;
}

} // namespace

std::string default_value(const Setting& s) {
  if (s.def) return s.def;
  switch (s.type) {
  case T::Bool:
  case T::Pick:    return s.choices[0].value;
  case T::Int:
  case T::Percent: return s.sentinel_value ? s.sentinel_value : std::to_string(s.lo);
  }
  return "";
}

std::string display_value(const Setting& s, const std::string& value) {
  const std::string v = value.empty() ? default_value(s) : value;
  switch (s.type) {
  case T::Bool:
  case T::Pick: {
    const int i = choice_index(s, v);
    // A value the table does not know is shown as it stands rather than
    // silently redrawn as something else: the file said it, and the player
    // should see what the file said.
    return i >= 0 ? s.choices[i].label : v;
  }
  case T::Int:
    if (s.sentinel_value && v == s.sentinel_value) return s.sentinel_label;
    return v;
  case T::Percent: {
    if (s.sentinel_value && v == s.sentinel_value) return s.sentinel_label;
    return std::to_string(percent_of(v)) + "%";
  }
  }
  return v;
}

std::string step_value(const Setting& s, const std::string& value, int dir, const SettingsHost& host) {
  const std::string v = value.empty() ? default_value(s) : value;
  if (s.type == T::Bool || s.type == T::Pick) {
    int i = choice_index(s, v);
    if (i < 0) i = 0;
    // Wraps: these lists are short, and a two-entry one has to wrap to be
    // usable at all. Skips anything the tier does not offer, and gives up
    // rather than looping if nothing is allowed.
    for (int n = 0; n < s.nchoices; ++n) {
      i = (i + (dir > 0 ? 1 : s.nchoices - 1)) % s.nchoices;
      if (host.value_allowed(s, s.choices[i].value)) return s.choices[i].value;
    }
    return v;
  }
  const bool at_sentinel = s.sentinel_value && v == s.sentinel_value;
  if (at_sentinel) {
    // The sentinel sits one step below the range: down from it does nothing,
    // up from it lands on lo.
    if (dir <= 0) return v;
    return s.type == T::Percent ? percent_str(s.lo) : std::to_string(s.lo);
  }
  const int cur = s.type == T::Percent ? percent_of(v) : std::atoi(v.c_str());
  int next = cur + dir * s.step;
  if (next < s.lo) {
    if (s.sentinel_value) return s.sentinel_value;
    next = s.lo;
  }
  if (next > s.hi) next = s.hi;
  // A value the file already held outside the menu's range is stepped from
  // where it is and clamped in, rather than jumped to an end.
  return s.type == T::Percent ? percent_str(next) : std::to_string(next);
}

} // namespace ds::sdl
