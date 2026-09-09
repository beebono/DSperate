// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The window RetroAchievements sees onto DS memory.
//
// Achievement authors write conditions against a flat address space that is
// not the DS's: rcheevos publishes it per console (rc_consoles.h), and for the
// DS it is three regions -- 4 MB of main RAM, a 12 MB hole that exists only so
// the DS and DSi maps line up, then the ARM9's data TCM. This translates that
// space to ours.
//
// Two properties matter more than speed here, though it is also fast (a bounds
// check and a memcpy from a host pointer -- no page tables, no Bus::io_read, so
// no write traps, no timing, no VRAM remap side effects; emulation cannot tell
// it happened):
//
//   * An address we do not back reads as ZERO, and reports itself as unbacked
//     so rcheevos can mark the achievements that use it unsupported. The
//     tempting alternative -- serving whatever the DS happens to have there --
//     is how you get false unlocks, because a condition written against DSi RAM
//     would silently compare against unrelated live data instead of being
//     disabled. drastic-nano's notes make the same point.
//   * The region table is taken from rcheevos at run time rather than copied
//     here, and checked against what we expect. If an rcheevos update ever
//     changes the DS map (giving the DSi hole real RAM, say) we want a loud
//     failure, not a quiet mis-mapping that moves every achievement's data.
#pragma once

#include <cstdint>
#include <string>

#include "core/types.h"

namespace ds { class NDS; }

namespace ds::cheevos {

class Memory {
public:
  // False with a reason in `err` if rcheevos' DS map is not the one this code
  // was written against -- see the header comment.
  bool attach(NDS& nds, std::string& err);
  bool attached() const { return regions_[0].host != nullptr; }

  // Reads `n` bytes at a RetroAchievements address into `dst`. Unbacked bytes
  // are written as zero. Returns how many of the bytes were actually backed,
  // which is the signal rcheevos wants: fewer than `n` means "this address is
  // not supported here", and it uses that to disable achievements rather than
  // to evaluate them against nonsense.
  u32 read(u32 address, u8* dst, u32 n) const;

  // Whether every byte of `[address, address + n)` is backed. For
  // rc_runtime_validate_addresses / rc_client's load-time address check.
  bool supported(u32 address, u32 n = 1) const;

  // The highest address the map describes, backed or not.
  u32 max_address() const { return max_address_; }

  // Adapters with the signatures rcheevos' two entry points want. `ud` is a
  // Memory*. Both are safe to call on an unattached Memory (everything reads
  // zero), because rcheevos may evaluate before a game is loaded.
  static u32 peek(u32 address, u32 num_bytes, void* ud);          // rc_runtime_peek_t
  static u32 read_memory(u32 address, u8* buffer, u32 num_bytes, void* ud);

private:
  // One entry per region rcheevos publishes, in its order. `host` null means
  // the region is real in the map but not backed by us (the DSi hole).
  struct Region {
    u32 start = 0, end = 0;   // inclusive RetroAchievements addresses
    u8* host = nullptr;
    u32 size = 0;
  };
  Region regions_[3];
  u32 max_address_ = 0;
};

// What DSperate tells the RetroAchievements server it is.
//
// This is load-bearing: the server answers a request it cannot attribute to a
// recognisable client with 403 unsupported_client, on every endpoint, including
// ones that need no account. A malformed product token therefore disables the
// whole feature with no other symptom, which is why it lives in one place and
// is covered by a test.
//
// The shape is a product token with no spaces and a numeric version, then
// rcheevos' own clause. Two rules: the version tracks DSperate's latest release
// tag (project() in CMakeLists.txt says 1.0.0 and is stale -- do not use it),
// and the product is always *ours*. Presenting another client's token would
// clear the same 403 and is not an option; see
// docs/retroachievements-scoping.md.
const char* user_agent();

} // namespace ds::cheevos
