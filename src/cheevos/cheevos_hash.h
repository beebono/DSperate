// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The RetroAchievements identity of a ROM. This is the whole of phase 1 in
// docs/retroachievements-scoping.md: RetroAchievements recognises a game by
// an MD5 over a specific set of bytes, and everything else in the feature
// (the session, the achievement set, unlocks) hangs off getting that one
// string right. Nothing here touches the network.
//
// The computation itself is rcheevos' (rcheevos/src/rhash/hash_rom.c); what
// is ours is feeding it our ROM bytes rather than letting it open the file,
// so that a ROM inside a zip hashes without being unpacked to disk, and so
// that the secure-area rewrite cannot reach the hash. See
// RomSource::read_unpatched for why that second one matters.
#pragma once

#include <string>

namespace ds::cart { class RomSource; }

namespace ds::cheevos {

// The RetroAchievements hash for a DS ROM: 32 lowercase hex characters in
// `out`, or false with a reason in `err`. `name` appears only in messages.
//
// Safe to call on any thread, and it does no IO of its own beyond touching
// `src` -- for a mapped source that means demand-paging a few hundred KB (the
// header, the two binaries, the icon block), not the whole image.
bool rom_hash(const cart::RomSource& src, const std::string& name,
              std::string& out, std::string& err);

} // namespace ds::cheevos
