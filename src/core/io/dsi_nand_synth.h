// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSiWare without a NAND dump (docs/dsiware-scoping.md 2.2). The launcher
// hand-off (NDS::dsi_hle_launch) starts a title the way the DSi Launcher
// does, and the title then mounts nand:/ for its own image and saves. This
// builds that NAND in memory: a formatted, encrypted filesystem under a
// made-up console, holding the title and the console's settings files
// generated from the user settings ([user] in the frontends' config).
//
// Nothing Nintendo-owned is on it: no launcher, no system titles, no
// TWLFontTable.dat, no certificates. Titles that read those get nothing.
#pragma once
#include "core/types.h"
#include "core/bios/freebios.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

// The console's region, as a DSi title sees it: HWINFO_S's region, language
// mask and launcher title ID, TWLCFG's country and language.
struct DsiRegion {
  u8   region = 1;        // 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR
  u8   country = 0x31;    // TWLCFG country code (USA)
  u8   language = 1;      // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es, 6 zh, 7 ko
  u8   language_mask = 0x26;
  char letter = 'E';      // the launcher's title ID ends in it (HNAE)
};

// The region a title runs in: one its header allows (0x1B0), preferring the
// one whose languages include the user's; the language is the user's when
// that region has it, else the region's own.
DsiRegion dsi_region_for(u32 header_region_flags, u8 user_language);

// The console files, whole (0x4000 bytes, 0xFF padded, as on a NAND).
struct DsiConsoleFiles {
  std::vector<u8> twlcfg;     // /shared1/TWLCFG0.dat and TWLCFG1.dat
  std::vector<u8> hwinfo_n;   // /sys/HWINFO_N.dat
  std::vector<u8> hwinfo_s;   // /sys/HWINFO_S.dat (unsigned: only the launcher checks)
  // /sys/TWLFontTable.dat when non-empty: the user's own. None can be made --
  // titles check its Nintendo RSA signature (a flipped signature byte stops
  // EA Sudoku; flipped font data and index hashes do not).
  std::vector<u8> font;
  // What a DSi direct boot copies into main RAM from them: the 0x154-byte
  // form NDS::load_dsi_boot_blobs takes.
  std::vector<u8> boot_blobs() const;
};
DsiConsoleFiles make_dsi_console_files(const bios::UserSettings& user, const DsiRegion& region, u64 console_id);

// The made-up console the synthesised NAND belongs to.
extern const u64 kSynthConsoleId;
extern const u8  kSynthEmmcCid[16];

// Replace `nand` with an in-memory NAND holding the console files and the
// DSiWare title `srl` (installed as content 00000000, with empty saves of the
// sizes its header asks for). `bios7i` is the DSi ARM7 BIOS (64 KB).
// Without a font the image watches the /sys directory's sectors
// (NandImage::watch_hit): a title reading them was looking for it.
bool build_synthetic_nand(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl,
                          const DsiConsoleFiles& files, std::string* err);

}  // namespace ds::io
