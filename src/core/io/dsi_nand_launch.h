// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSiWare already installed on a NAND dump, started without the DSi Menu.
//
// Launching a title from the DSi Menu reads its .app through the emulated
// eMMC, which is slow on a handheld. The launcher hand-off (NDS::dsi_hle_launch)
// skips boot2 and the menu: the title's .app is read out of the NAND once, put
// in the slot, and handed over with the real NAND behind it (its saves, its
// settings, its font).
//
// A "shortcut" is how a game list reaches one: a small file named
// "<banner title>.dspr.nds" in the games folder holding a DSPR marker, the
// title ID and the identity of the NAND it came from. The suffix marks the
// files DSperate made, so they are the only ones it ever removes.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

struct NandTitle {
  u32 title_lo = 0;          // 0x4B533345
  std::string code;          // "KS3E"
  std::string name;          // the banner's title (English where there is one), UTF-8, one line
  u32 content_id = 0;        // its CONTENT/xxxxxxxx.APP
};

// The DSiWare titles (00030004) installed on `nand`, with their banner names.
std::vector<NandTitle> nand_dsiware_titles(NandImage& nand, const u8* bios7i);

// A title's .app, whole.
bool nand_read_title_app(NandImage& nand, const u8* bios7i, u32 title_lo, std::vector<u8>& srl, u32& content_id, std::string* err);

// What a DSi direct boot copies into main RAM from the NAND (the form
// NDS::load_dsi_boot_blobs takes, 0x154 bytes): the newer TWLCFG's 0x88..0x1AF,
// HWINFO_N's 0x88..0x9B and HWINFO_S's 0x88..0x9F (melonDS SetupDirectBoot).
bool nand_boot_blobs(NandImage& nand, const u8* bios7i, std::vector<u8>& out, std::string* err);

// ---- shortcuts ----------------------------------------------------------------

inline constexpr const char* kShortcutSuffix = ".dspr.nds";

struct NandShortcut {
  u32 title_lo = 0, title_hi = 0x00030004;
  u8  cid[16] = {};
  u64 console_id = 0;
  // Whether it names a title on this NAND (the same eMMC CID and console ID).
  bool from(const NandImage& nand) const;
};

// Whether `path` ends in .dspr.nds (any case).
bool is_shortcut_name(const std::string& path);
// Reads one; false when the file is not a shortcut.
bool read_shortcut(const std::string& path, NandShortcut& out);

struct ShortcutSync {
  int written = 0, removed = 0;
  std::vector<std::string> notes;
};

// `enabled`: a shortcut in `dir` for every title on `nand`, and none for a
// title it does not have (a shortcut from another NAND counts as that).
// Otherwise every .dspr.nds file in `dir` is removed. Only .dspr.nds files are
// ever written or removed. A title's file is named from its banner, made safe
// for a file system; two titles with one name get their game codes appended.
ShortcutSync sync_shortcuts(const std::string& dir, NandImage* nand, const u8* bios7i, bool enabled);

}  // namespace ds::io
