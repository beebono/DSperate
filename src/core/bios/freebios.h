// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Built-in replacements for the images a DS boots from, used when the user
// has no dumps. Neither is Nintendo's: the BIOS is FreeBIOS (Gilead Kutnick,
// BSD-3-Clause, see LICENSE.freebios), a SWI table and IRQ dispatcher with no
// boot code, and the firmware is a data-only image with default user
// settings. Both only work with direct boot -- there is nothing to boot the
// DS menu from -- and the SWI routines are not cycle-matched to Nintendo's,
// so timing-sensitive comparisons (scene hashes, replays) need real dumps.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::bios {

// FreeBIOS images. Smaller than the BIOS regions they fill (the rest is zero).
// The ARM9 image leaves the logo area at 0x20 blank, and the ARM7 image has
// no KEY1 table at 0x30 -- see NDS::setup_direct_boot and cart.cpp.
extern const u8 kFreeBios9[];
extern const u32 kFreeBios9_len;
extern const u8 kFreeBios7[];
extern const u32 kFreeBios7_len;

// What the generated firmware exposes to games as the console's owner: the
// fields the DS menu's settings pages would have written. Nickname and
// message are UTF-16 in the firmware; here they are plain strings of which
// the first 10 / 26 characters are used, each byte one code unit.
struct UserSettings {
  std::string nickname = "DSperate";
  std::string message;
  u8 birthday_month = 1;   // 1..12
  u8 birthday_day = 1;     // 1..31
  u8 favourite_colour = 0; // 0..15 (GBATEK order: grey, brown, red, pink, orange, yellow, lime, green, ...)
  u8 language = 1;         // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es
};

// A 256 KB firmware image: header (DS Lite, W006 wifi, generated identifier
// "DSPR"), two user-settings copies at 0x3FE00 with valid checksums, three
// wifi access-point blocks. No code. Touch calibration is left for
// NDS::normalise_touch_calibration, as with a real dump.
std::vector<u8> generate_firmware(const UserSettings& user);

// The DS firmware CRC16 (GBATEK "Firmware Header").
u16 crc16(const u8* data, u32 len, u16 start);

} // namespace ds::bios
