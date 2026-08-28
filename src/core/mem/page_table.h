// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstring>
#include "core/types.h"
#include "core/profile.h"

namespace ds::mem {

// The single memory abstraction shared by the interpreter, the JIT and DMA.
//
// Design: one flat table of 8-byte tagged entries,
// one per 2 KB guest page, covering the whole 32-bit guest address space so that
// no address ever needs masking before lookup. The entry stores a *pre-biased*
// host base so that `host_base + guest_addr` addresses the byte directly; the
// low 62 bits hold that base >> 2, and the top two bits are tags:
//
//   bit 63  CODE     page contains translated code -> stores must check for SMC
//   bit 62  SPECIAL  not plain RAM for *writes*    -> MMIO / ROM / write-protect
//
// Recovering the pointer is `entry << 2`, which discards both tags at once, so
// loads from a CODE page are exactly as fast as loads from plain RAM and a pure
// MMIO page (base 0, SPECIAL set) falls to the slow path with no extra branch.
//
// Host pointers must therefore be 4-byte aligned and the biased value must fit
// in 62 bits; both are asserted at map time.

// Self-modifying-code notification: every store path that lands on a page
// tagged CODE reports the host bytes it wrote. The recompiler installs the
// hook; without one the call is a null check.
extern void (*code_write_hook)(u8* host, u32 len);
inline void code_written(u8* host, u32 len) { if (code_write_hook) code_write_hook(host, len); }
// A store landing on a CODE page. Silent-store elimination first: a value
// identical to what is already there changes no translation, so it is not
// reported at all -- a game re-uploading the same overlay, or clearing a
// region that is already clear, costs a compare and nothing else (DraStic's
// second SMC filter; the third, whether a block actually covers the bytes,
// is the hook's business).
struct CodeStoreStats { u64 silent = 0, changed = 0; };
extern CodeStoreStats code_store_stats;
inline void store_code(u8* host, const void* v, u32 len) {
  if (std::memcmp(host, v, len) == 0) { ++code_store_stats.silent; return; }
  std::memcpy(host, v, len);
  ++code_store_stats.changed;
  code_written(host, len);
}

constexpr u32 PAGE_SHIFT = 11;
constexpr u32 PAGE_SIZE  = 1u << PAGE_SHIFT;      // 2 KB
constexpr u32 PAGE_COUNT = 1u << (32 - PAGE_SHIFT); // 2 Mi entries = 16 MiB of table

using Entry = u64;

constexpr Entry TAG_CODE    = Entry{1} << 63;
constexpr Entry TAG_SPECIAL = Entry{1} << 62;
constexpr Entry BASE_MASK   = ~(TAG_CODE | TAG_SPECIAL);

enum PageFlags : u32 {
  PAGE_READABLE = 1u << 0,   // direct loads allowed
  PAGE_WRITABLE = 1u << 1,   // direct stores allowed
  PAGE_MMIO     = 1u << 2,   // no host backing at all
};

class PageTable {
public:
  PageTable();
  ~PageTable();
  PageTable(const PageTable&) = delete;
  PageTable& operator=(const PageTable&) = delete;

  // Map `size` bytes of guest space starting at `guest` onto `host`.
  // `host` may alias another mapping (mirrors, shared WRAM views). `size` and
  // `guest` must be PAGE_SIZE aligned.
  void map(u32 guest, u32 size, u8* host, u32 flags);
  void map_mmio(u32 guest, u32 size);
  void unmap(u32 guest, u32 size);
  // Set the region to `hosts` (one pointer per page, nullptr = unmapped,
  // all with `flags`), touching only the entries that change: the index and
  // code tags of unchanged pages stay as they are.
  void remap(u32 guest, u32 size, u8* const* hosts, u32 flags);

  void set_code(u32 guest, u32 size, bool is_code);

  // Write trap: stores to every mapped page in [guest, guest+size) take the
  // slow path (the entry keeps its base, so loads stay direct) until the trap
  // is lifted again. Only entries with a host base are touched. The region
  // must hold no read-only or MMIO pages: their SPECIAL bit means something
  // else and lifting the trap would clear it.
  void set_write_trap(u32 guest, u32 size, bool on);

  // Tag (or untag) every entry in the low 256 MB of guest space that maps the
  // given 2 KB host page. Code pages are tracked by host address so that the
  // other CPU's view and DMA see the same tag; `map` re-applies tags through
  // `code_query` so remapping keeps them.
  void set_code_host(const u8* host_page, bool is_code);
  static bool (*code_query)(const u8* host_page);

  // Fast-path helpers. Return nullptr when the access must take the slow path.
  inline u8* read_ptr(u32 addr) const {
    Entry e = table_[addr >> PAGE_SHIFT];
    u64 base = e << 2;
    return base ? reinterpret_cast<u8*>(base + addr) : nullptr;
  }
  inline u8* write_ptr(u32 addr, bool* is_code) const {
    if (prof::enabled) {          // census: what a write-path dirty bit must intercept
      const u32 r = addr >> 24;
      if (r == 5) prof::add(prof::C_W_PALETTE, 1);
      else if (r == 7) prof::add(prof::C_W_OAM, 1);
    }
    Entry e = table_[addr >> PAGE_SHIFT];
    if (e & TAG_SPECIAL) return nullptr;
    *is_code = (e & TAG_CODE) != 0;
    u64 base = e << 2;
    return base ? reinterpret_cast<u8*>(base + addr) : nullptr;
  }

  Entry  entry(u32 addr) const { return table_[addr >> PAGE_SHIFT]; }
  Entry* raw()                 { return table_; }
  const Entry* raw() const     { return table_; }

private:
  Entry* table_;   // PAGE_COUNT entries, mmap'd: untouched pages cost no RSS
  // Reverse index for set_code_host: host page number -> the guest pages (low
  // 256 MB) mapping it, maintained by map/unmap. An open-addressing table of
  // host pages whose values head intrusive lists threaded through `next`;
  // no allocation on the remap path.
  struct HostIndex;
  HostIndex* index_;
  void index_insert(u32 guest_page, Entry e);
  void index_remove(u32 guest_page, Entry e);
};

} // namespace ds::mem
