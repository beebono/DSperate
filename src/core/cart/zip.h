// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reading a ROM out of a zip. The cart reads its image through a RomSource
// (rom_source.h), so an archive is only a different way of producing those
// bytes: a stored entry is mapped where it lies, a deflated one is inflated
// once to a file beside the archive (zip_cache.h) and that is mapped.
// Nothing downstream can tell the difference.
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

// The image entry the archive is taken to hold -- see find_rom().
struct ZipEntry {
  std::string name;
  u16  method = 0;          // 0 stored, 8 deflated
  u64  data_off = 0;        // payload offset in the archive
  u64  csize = 0, usize = 0;
  u32  crc32 = 0;           // of the uncompressed bytes, as the archive declares it
  // A .cia: a DSiWare title wrapped in a CIA container rather than a bare
  // image. Its bytes are not a DS header, so nothing here peeks into it and
  // the cart path must not map it as a ROM -- io/dsi_title_install.h unwraps
  // it to the SRL inside first.
  bool cia = false;
  bool stored() const { return method == 0; }
};

// Picks the image entry out of `zip`, without reading its payload beyond the
// header peek described below. False with a reason in `err`.
//
// The extensions taken to hold an image are .nds, .dsi and .srl (a bare ROM
// or DSiWare SRL) and .cia (the same SRL in a CIA container). A bare entry is
// always preferred over a .cia, since it needs no unwrapping.
bool find_rom(const u8* zip, size_t size, ZipEntry& entry, std::string& err);

// The first `n` uncompressed bytes of `entry` (fewer if it is shorter), for a
// caller that needs to look at a header without paying for the whole image.
// Returns how many were produced. The CRC is not checked: this is a peek, and
// only inflate_entry vouches for a complete stream.
size_t peek_entry(const u8* zip, size_t size, const ZipEntry& entry, u8* out, size_t n);

// Produces the entry's uncompressed bytes in order through `sink` (which
// returns false to abort), calling `progress` every megabyte or so with the
// bytes produced and the total. Verifies the CRC32 at the end: a stream that
// ends early, overruns, or does not match is a corrupt archive. False with
// a reason in `err`.
using ZipSink = bool (*)(void* user, const u8* data, size_t n);
using ZipProgress = void (*)(void* user, u64 done, u64 total);
bool inflate_entry(const u8* zip, size_t size, const ZipEntry& entry, ZipSink sink, void* sink_user,
                   ZipProgress progress, void* progress_user, std::string& err);

// The reflected CRC-32 zips use, for the verification above and the cache tag.
u32 crc32_update(u32 crc, const u8* data, size_t n);

// Extracts the image from `zip` into `out` (find_rom + inflate_entry into a
// vector). The in-memory path, for a filesystem that cannot map and tests.
//
// With one entry, that one. With several, the pick follows the game
// database: entries whose game code is in save_list.inc beat entries that are
// not (homebrew, translations and hacks are not listed, so a zip of those
// still loads), then the highest header revision wins -- the latest revision
// of a game -- and a tie falls back to the archive's own order. The chosen
// entry's name is written to `chosen` when it is not null. A .cia is only
// reached for when there is no bare entry at all, and cannot join that
// comparison: its first bytes are a container header, not a DS one.
//
// Note the database says "this game code is known", not "this dump is good":
// a bad dump with an intact header still matches.
//
// False on any failure, with a human-readable reason in `err`. A corrupt
// archive must fail loudly rather than hand back a short or garbage image.
// A CIA is refused unless `allow_cia`: its bytes are a container, and a
// caller that has not asked for one would load the header as a DS ROM.
// io/dsi_title_install.h is the caller that asks, and unwraps it.
bool extract_rom(const u8* zip, size_t size, std::vector<u8>& out, std::string& err,
                 std::string* chosen = nullptr, bool allow_cia = false, bool* was_cia = nullptr);

// The reason a .cia entry is refused where only a bare image will do.
std::string cia_refusal(const std::string& name);

} // namespace ds::cart
