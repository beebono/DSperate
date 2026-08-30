// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <map>
#include <string>

namespace ds::sdl {

// INI-style settings: `[section]` headers, `key = value` lines, `#` or `;`
// comments. Values are kept as strings and typed at the point of use, so the
// same map serves the global file, a per-game override and the command line
// (later loads win, in that order).
//
//   ~/.config/dsperate/dsperate.ini          (or $XDG_CONFIG_HOME/dsperate/)
//   ~/.config/dsperate/games/<rom name>.ini  overrides for one ROM file
//   ~/.config/dsperate/games/<GAMECODE>.ini  overrides for one title (any file)
//
// Keys are addressed as "section.key".
class Config {
public:
  static std::string dir();                        // the config directory, created on demand
  static std::string global_path() { return dir() + "/dsperate.ini"; }
  // The per-game files, in load order: the title-ID one first, then the one
  // named after the ROM file (its basename without the extension), so the
  // filename wins. Both live in games/; `rom` may be a full path.
  static std::string game_path_code(const char code[4]);
  static std::string game_path_rom(const std::string& rom);

  bool load(const std::string& path);              // merge a file; false if it does not exist
  void set(const std::string& key, const std::string& value) { kv_[key] = value; }
  bool has(const std::string& key) const { return kv_.count(key) != 0; }

  std::string str(const std::string& key, const std::string& def = "") const;
  int    num(const std::string& key, int def) const;
  double real(const std::string& key, double def) const;
  bool   flag(const std::string& key, bool def) const;

  // Rewrites one `key = value` in `path`, adding the section or the file if
  // needed; used to remember a layout picked with a hotkey.
  static bool store(const std::string& path, const std::string& key, const std::string& value);
  // Writes the commented default file if there is none.
  static void write_default(const std::string& path, bool force = false);

private:
  std::map<std::string, std::string> kv_;
};

} // namespace ds::sdl
