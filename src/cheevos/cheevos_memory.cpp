// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_memory.h"

#include <cstring>

#include "core/mem/bus.h"
#include "core/nds.h"
#include "rc_consoles.h"
#include "rc_version.h"

namespace ds::cheevos {

const char* user_agent() {
  // "DSperate/<tag> rcheevos/<ver>". Built once; rcheevos' half comes from the
  // vendored snapshot so it cannot disagree with the library we linked.
  static const std::string ua = std::string("DSperate/") + DSPERATE_CHEEVOS_VERSION +
                                " rcheevos/" + RCHEEVOS_VERSION_STRING;
  return ua.c_str();
}

bool Memory::attach(NDS& nds, std::string& err) {
  err.clear();
  for (Region& r : regions_) r = Region{};
  max_address_ = 0;

  const rc_memory_regions_t* map = rc_console_memory_regions(RC_CONSOLE_NINTENDO_DS);
  if (!map) { err = "rcheevos has no memory map for the DS"; return false; }

  // The map this code was written against: main RAM, the DSi-only hole, data
  // TCM. Anything else and we would be mapping the wrong bytes to the addresses
  // achievements are written against, so refuse rather than guess.
  if (map->num_regions != 3) {
    err = "rcheevos' DS map has " + std::to_string(map->num_regions) +
          " regions, not the 3 this was written against";
    return false;
  }

  mem::Bus& bus = nds.bus;
  struct Expect { u32 size; u8* host; const char* what; };
  const Expect expect[3] = {
    {mem::Bus::MAIN_RAM_SIZE, bus.main_ram.get(), "main RAM"},
    {0xC00000,                nullptr,            "the DSi-only hole"},
    {mem::Bus::DTCM_SIZE,     bus.dtcm.get(),     "data TCM"},
  };

  for (u32 i = 0; i < 3; ++i) {
    const rc_memory_region_t& rr = map->region[i];
    const u32 size = rr.end_address - rr.start_address + 1;
    if (size != expect[i].size) {
      err = std::string("rcheevos' DS region ") + std::to_string(i) + " (" + expect[i].what +
            ") is " + std::to_string(size) + " bytes, not " + std::to_string(expect[i].size);
      return false;
    }
    regions_[i].start = rr.start_address;
    regions_[i].end = rr.end_address;
    regions_[i].size = size;
    // The hole stays unbacked on purpose even though the map calls it a real
    // region: RC_MEMORY_TYPE_UNUSED is padding, and we must not answer for it.
    regions_[i].host = rr.type == RC_MEMORY_TYPE_UNUSED ? nullptr : expect[i].host;
    if (rr.type != RC_MEMORY_TYPE_UNUSED && !regions_[i].host) {
      err = std::string("no buffer for ") + expect[i].what;
      return false;
    }
    if (rr.end_address > max_address_) max_address_ = rr.end_address;
  }
  return true;
}

u32 Memory::read(u32 address, u8* dst, u32 n) const {
  u32 backed = 0;
  while (n) {
    const Region* hit = nullptr;
    for (const Region& r : regions_) {
      if (r.size && address >= r.start && address <= r.end) { hit = &r; break; }
    }
    if (!hit) {
      // Past the end of the map entirely. Zero the rest and stop looking: the
      // regions are contiguous, so nothing further along can be backed.
      std::memset(dst, 0, n);
      return backed;
    }
    const u32 off = address - hit->start;
    const u32 take = n < hit->size - off ? n : hit->size - off;
    if (hit->host) {
      std::memcpy(dst, hit->host + off, take);
      backed += take;
    } else {
      std::memset(dst, 0, take);
    }
    dst += take; address += take; n -= take;
  }
  return backed;
}

bool Memory::supported(u32 address, u32 n) const {
  if (n == 0) return true;
  // Cheap and allocation-free: ask read() about it through a small stack
  // buffer, in chunks, and require every byte to come back backed.
  u8 scratch[64];
  while (n) {
    const u32 take = n < sizeof scratch ? n : static_cast<u32>(sizeof scratch);
    if (read(address, scratch, take) != take) return false;
    address += take; n -= take;
  }
  return true;
}

u32 Memory::peek(u32 address, u32 num_bytes, void* ud) {
  const Memory* m = static_cast<const Memory*>(ud);
  u8 b[4] = {};
  if (num_bytes > sizeof b) num_bytes = sizeof b;
  if (m) m->read(address, b, num_bytes);
  // Little-endian, which is both the DS's byte order and rcheevos' convention.
  u32 v = 0;
  for (u32 i = 0; i < num_bytes; ++i) v |= static_cast<u32>(b[i]) << (8 * i);
  return v;
}

u32 Memory::read_memory(u32 address, u8* buffer, u32 num_bytes, void* ud) {
  const Memory* m = static_cast<const Memory*>(ud);
  if (!m) { std::memset(buffer, 0, num_bytes); return 0; }
  return m->read(address, buffer, num_bytes);
}

} // namespace ds::cheevos
