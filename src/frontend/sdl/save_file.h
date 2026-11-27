// SPDX-License-Identifier: GPL-3.0-or-later
// Which file a game's battery save lives in. Three formats hold the same raw
// image of the save chip:
//   .sav  ours (and melonDS's): the bare image
//   .dsv  DraStic's (DeSmuME's before it): the image and DeSmuME's 122-byte
//         footer, which DraStic writes too. DraStic reads the chip's size
//         from the front of the file and adds the footer itself when the
//         file lacks one, so the bare .dsv a forced save_file = dsv makes
//         here is read there (checked against DraStic's NSMB save, 2026-11-27)
//   .srm  RetroArch's (melonDS DS, mGBA, ...): the bare image, or the same in
//         RetroArch's rzip when its save_file_compression is on
// emu.save_file (slot2.gba_save_file for the GBA game) forces one; under auto
// a lone file is used, and several are offered before the game starts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ds::sdl {

enum class SaveKind : uint8_t { Sav, Dsv, Srm };

const char* save_ext(SaveKind k);        // ".sav"
const char* save_kind_name(SaveKind k);  // "sav", the setting's word
// "sav" / "dsv" / "srm" (any case, a leading dot allowed); false for "auto" and anything else.
bool parse_save_kind(const std::string& v, SaveKind& out);

struct SaveFile {
  std::string path;
  SaveKind kind = SaveKind::Sav;
  uint64_t bytes = 0;       // on disk
  int64_t mtime = 0;        // seconds since the epoch
  bool compressed = false;  // RetroArch rzip
  bool blank = false;       // every byte the same 0x00 or 0xFF: nothing saved yet
  bool beside_rom = false;  // found beside the ROM, not in the saves folder
};

// The files that exist at <stem><ext> for each stem (the saves folder's
// first, then the ROM's, duplicates dropped) and each kind, in that order.
std::vector<SaveFile> find_saves(const std::vector<std::string>& stems, const std::vector<SaveKind>& kinds);

// Newest file with something in it; else the newest; -1 for none.
int newest_save(const std::vector<SaveFile>& saves);

enum class ReadResult { Ok, Missing, Error };
// The save's bytes, a RetroArch rzip inflated. Error carries the reason in `err`.
ReadResult read_save_file(const std::string& path, std::vector<uint8_t>& out, std::string& err);

bool is_rzip(const uint8_t* p, size_t n);
bool is_rzip_file(const std::string& path);
// False on a malformed stream (or a zstd one, RetroArch's version 2, which is not read).
bool rzip_decode(const uint8_t* p, size_t n, std::vector<uint8_t>& out);

} // namespace ds::sdl
