// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cheat/ar_engine.h"

#include <string>
#include <vector>

namespace ds::cheat {

// The R4 `usrcheat.dat` cheat database, the format every DS cheat collection
// is published in and the one DraStic and melonDS both read.
//
//   header  "R4 CheatCode\0\1\0\0", a 0x3C-byte description, then the entry
//           list at 0x100: {game code, ROM header checksum, offset, 0} per
//           entry, terminated by a zero game code.
//   entry   the game's name, then a flags word whose low 24 bits count the
//           items, then eight master-code words, then the items.
//   item    a flags word (low 24 bits a length, bit 24 "enabled" or "only one
//           of these", bit 28 "this is a category"), a name and a description
//           as NUL-terminated strings, padding to a word, then the payload:
//           a word count and that many code words, or -- for a category --
//           nothing, the next `length` codes belonging to it.
//
// The file is read whole and parsed from memory: it is a few megabytes, the
// entries are scattered across it by absolute offset, and every field wants
// bounds-checking against the same buffer.
struct Group {
  std::string name, description;
  // The file's "only one code enabled" flag: the codes in this group are a
  // set of alternatives (difficulty, a character, a language), not switches.
  bool exclusive = false;
};

// One game's cheats. `codes` is flat -- a group is a label on its members,
// which is what a menu wants -- and `groups` holds the categories in the
// order they appeared. A code with group -1 sits outside any category.
struct GameCheats {
  u32 game_code = 0;      // the four ASCII characters of the ROM's game code
  u32 checksum = 0;       // of the ROM header; distinguishes revisions and regions
  std::string name;
  std::vector<Group> groups;
  std::vector<Code> codes;
};

// The checksum the database keys revisions by: the complement of a CRC-32
// over the ROM's first 512 bytes.
u32 header_checksum(const u8* header, size_t n = 512);

// The whole job a frontend wants: read the database, work out which entry
// this ROM file is, and hand back its cheats. The ROM is opened only for its
// first 512 bytes -- the game code lives at offset 0x0C and the checksum is
// over the header -- so this costs nothing next to loading the ROM itself.
// False with `err` set if the database or the ROM cannot be read; false with
// `err` empty if both were fine and the game simply is not in the database,
// which is the common case and not an error worth showing.
bool load_for_rom(const std::string& db_path, const std::string& rom_path, GameCheats& out, std::string& err);

// The same from a header already in hand -- 512 bytes at offset 0 of the ROM
// image. This is the form a frontend should use once the cart is loaded: the
// path may name a zip, whose first 512 bytes are the archive's, not the
// game's, and every ROM would then miss the database.
bool load_for_header(const std::string& db_path, const u8 header[512], GameCheats& out, std::string& err);

class Database {
public:
  // Reads and indexes the file. False (with `err` set) if it is missing, too
  // short, or not a usrcheat database.
  bool open(const std::string& path, std::string& err);

  const std::string& name() const { return name_; }        // the database's own description
  bool has(u32 game_code) const;
  size_t game_count() const { return index_.size(); }

  // Every entry filed under this game code -- usually one, but a game with
  // several revisions has one per checksum. Entries that fail to parse are
  // left out and named in `err`; the rest are still returned.
  std::vector<GameCheats> entries_for(u32 game_code, std::string& err) const;
  // The entry whose checksum matches, else the first one. False if the game
  // is not in the database at all.
  bool best_entry(u32 game_code, u32 checksum, GameCheats& out, std::string& err) const;

  // The game codes present, for tooling that wants to sweep the whole file.
  std::vector<u32> game_codes() const;

private:
  struct Entry { u32 game_code, checksum, offset; };
  std::vector<u8> file_;
  std::string name_;
  std::vector<Entry> index_;
  bool parse_entry(const Entry& e, GameCheats& out, std::string& err) const;
};

} // namespace ds::cheat
