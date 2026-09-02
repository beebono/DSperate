// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reading a ROM out of a zip. The cart holds the whole image in memory
// anyway (Cart keeps a std::vector<u8> and pads it to a power of two), so an
// archive is only a different way of producing those bytes -- there is no
// streaming path to fight, and nothing downstream can tell the difference.
//
// Only what a ROM archive needs: stored and deflated entries, no zip64, no
// encryption, no writing. The DEFLATE core is vendored miniz (miniz/); the
// container parsing here is ours so every offset is bounded against the
// buffer we were handed.
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cart {

// The local-file-header magic, "PK\x03\x04". Sniffed rather than trusting the
// extension: launchers hand over .ZIP, and some hand over extensionless
// temporary files.
bool is_zip(const u8* data, size_t size);

// Extracts the ROM from `zip` into `out`.
//
// With one .nds entry, that one. With several, the pick follows the game
// database: entries whose game code is in save_list.inc beat entries that are
// not (homebrew, translations and hacks are not listed, so a zip of those
// still loads), then the highest header revision wins -- the latest revision
// of a game -- and a tie falls back to the archive's own order. The chosen
// entry's name is written to `chosen` when it is not null.
//
// Note the database says "this game code is known", not "this dump is good":
// a bad dump with an intact header still matches.
//
// False on any failure, with a human-readable reason in `err`. A corrupt
// archive must fail loudly rather than hand back a short or garbage image.
bool extract_nds(const u8* zip, size_t size, std::vector<u8>& out, std::string& err,
                 std::string* chosen = nullptr);

} // namespace ds::cart
