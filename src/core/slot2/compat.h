// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DS games that read a GBA game in slot 2 ("dual-slot mode": Pal Park,
// unlockables, imported data), and which GBA games each one looks for.
// Codes are the first three letters of the game code, region left off:
// a game that cares about the region checks it itself.
#pragma once
#include "core/types.h"

#include <cstddef>

namespace ds::slot2 {

struct Linkage {
  const char* ds;            // three letters of the DS game code
  const char* const* gba;    // three-letter GBA codes it accepts, null-terminated
};

// The entry for a DS game code (four letters; the region is ignored), or
// null when the game reads no GBA game.
const Linkage* linkage_for(const char ds_code[4]);
// Whether `gba_code` (four letters) is one the entry accepts.
bool accepts(const Linkage& link, const char gba_code[4]);
// Whether the DS game (four letters; region ignored) is played with the
// Guitar Grip in slot 2.
bool uses_guitar_grip(const char ds_code[4]);

} // namespace ds::slot2
