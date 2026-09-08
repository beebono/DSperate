// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::sdl {

// The settings the pause menu offers, as tables rather than code: a row is one
// entry here, and the menu knows nothing about what any of them mean.
//
// These are deliberately a subset of the config file. Everything that is
// decided once (the tier, the window size, real-time scheduling), everything
// that only makes sense while debugging (the JIT switch, the CPU interleave)
// and everything with no visible effect stays ini-only. A menu that lists
// every key is a menu nobody can find anything in.

// An ini value and what the player reads. The file keeps its own vocabulary;
// only the menu renames -- "mean" is DEFAULT, "linear" is BILINEAR.
struct Choice { const char* value; const char* label; };

enum Flag : u8 {
  FlagLive    = 0,        // takes effect as soon as it is set
  FlagReopen  = 1u << 0,  // needs the display closed and opened again
  FlagRestart = 1u << 1,  // only read at startup; the row says so
  FlagInexact = 1u << 2,  // trades accuracy for speed; drawn as a warning
};

// Why a row might be switched off. Not all of these are "another key has this
// value" -- some read the live layout, some the display tier -- so the menu
// asks the host rather than resolving them itself.
enum class Dep : u8 {
  None,
  FrameskipMode,      // emu.frameskip > 0: adaptive-or-fixed means nothing at 0
  PanelEffects,       // the tier has panel pixels at all (not the display engine)
  GridSeam,           // PanelEffects, and video.linear off, which overrides them
  Chunky,             // video.linear off (chunky survives at DS resolution)
  ChunkyCell,         // Chunky, and video.chunky not off
  Windowed,           // the tier does not own the panel outright
  Pip,                // the layout is pip
  PipTouchHold,       // Pip, and the inset is not fully opaque (it is a fade timer)
  Dominant,           // the layout is dominant_v or dominant_h
  DominantThreshold,  // Dominant, and dominant_ratio is auto
};

struct Setting {
  const char* key;                 // "video.linear"
  const char* label;               // "BILINEAR"
  // Text is free-form: `lo` is how many characters the firmware keeps.
  enum class Type : u8 { Bool, Pick, Int, Percent, Text } type;
  const Choice* choices; u8 nchoices;
  // Int and Percent. These are the menu's bounds, not the file's: the file
  // accepts more, and a value already in it outside this range is shown and
  // left alone until the row is moved.
  int lo, hi, step;
  // A value one step below `lo` that means something other than a number.
  const char* sentinel_value;      // what goes in the file ("auto", "0")
  const char* sentinel_label;      // what the player reads ("AUTO", "UNLIMITED")
  // What the frontend uses when the key is absent. It has to be stated rather
  // than assumed to be the first choice or the bottom of the range: the menu
  // would otherwise show a value the emulator is not running with, which is
  // worse than showing nothing.
  const char* def;
  u8 flags;
  Dep depends;
  const char* note;                // one line, shown under the list
};

// What the menu needs from the frontend. Implemented in main.cpp, which is the
// only place that knows about the NDS, the Display and the config file; this
// keeps menu.cpp free of all three, the way the cheats page is free of the
// cheat engine.
struct SettingsHost {
  virtual ~SettingsHost() = default;
  // The value in the config now, or "" if the key is unset (the caller then
  // falls back to the table's own first choice / lo).
  virtual std::string get(const char* key) const = 0;
  // Apply it, then remember it. Where that goes -- the global file or this
  // game's -- is the host's business; see save_per_game().
  virtual void set(const char* key, const std::string& value) = 0;
  virtual bool enabled(const Setting& s) const = 0;
  // Why not, for the row to say. "" when it is enabled.
  virtual const char* disabled_reason(const Setting& s) const = 0;
  // Whether a choice is offered at all: the display-engine tier draws chunky
  // cells in its scaler and can only do their mean, so the rest are not shown
  // there rather than shown and ignored.
  virtual bool value_allowed(const Setting& s, const char* value) const = 0;
  // Anything expensive a change asked for, now that the player has finished
  // asking. Stepping LCD GRID from 0 to 50 is five presses, and reopening the
  // display on each of them would flicker the window five times; the menu
  // calls this when the selection leaves the row or the page instead.
  virtual void commit() = 0;
  virtual bool save_per_game() const = 0;
  virtual void set_save_per_game(bool on) = 0;
  // False when there is no game in the slot, so there is no per-game file to
  // save to and the toggle is not offered.
  virtual bool has_game() const = 0;

  // The Controls page. Bindings are not Settings: they have no range and no
  // list of choices, and the value comes from the player pressing the thing
  // they want rather than from stepping through possibilities.
  //
  // A row is a config key ("keys.a", "padhotkeys.pause") and the label the
  // page shows for it. `pad` picks the column: the page defaults to it when a
  // controller is plugged in, because on a handheld that is the only input.
  struct Binding { std::string key, label, value; };
  virtual int binding_count(bool pad) const = 0;
  virtual Binding binding(bool pad, int i) const = 0;
  virtual bool has_pad() const = 0;
  // Start listening for the next thing pressed. The frontend swallows it
  // rather than acting on it -- otherwise rebinding Quit would quit.
  virtual void begin_capture(bool pad) = 0;
  virtual void cancel_capture() = 0;
  virtual bool capturing() const = 0;
  // Non-empty once something was pressed: the caller binds it to `key` and
  // the capture ends.
  virtual std::string take_capture() = 0;
  virtual void bind(const std::string& key, const std::string& value) = 0;
  // Put a whole column back to the built-in layout.
  virtual void reset_bindings(bool pad) = 0;
  // Bindings that shadow one another, one line each; empty when there are none.
  virtual std::vector<std::string> collisions() const = 0;

  // False when a real firmware dump is in use, in which case [user] is not
  // read at all -- the dump's own pages win and the DS menu edits them. The
  // page says so rather than accepting changes that would do nothing.
  virtual bool user_settings_used() const = 0;
};

// The pages. Each is terminated by a row with a null key.
extern const Setting kEmuSettings[];
extern const Setting kVideoSettings[];
extern const Setting kLayoutSettings[];
extern const Setting kUserSettings[];
int settings_count(const Setting* table);

// What the row shows on the right: the label for a choice, "50%", "UNLIMITED".
std::string display_value(const Setting& s, const std::string& value);
// The value one step in `dir`, clamped at the ends rather than wrapping for
// numbers, and wrapping for a short list of choices. Skips choices the host
// does not allow.
std::string step_value(const Setting& s, const std::string& value, int dir, const SettingsHost& host);
// The value a row falls back to when the key is unset.
std::string default_value(const Setting& s);

} // namespace ds::sdl
