// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::mem {

// The single memory abstraction shared by the interpreter, the JIT and DMA.
//
// Design (docs/ARCHITECTURE.md §2): one flat table of 8-byte tagged entries,
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

  void set_code(u32 guest, u32 size, bool is_code);

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
};

} // namespace ds::mem
