// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/page_table.h"


#include <cassert>
#include <cstring>
#include <sys/mman.h>

namespace ds::mem {

void (*code_write_hook)(u8* host, u32 len) = nullptr;
CodeStoreStats code_store_stats;
bool (*PageTable::code_query)(const u8* host_page) = nullptr;

static constexpr size_t TABLE_BYTES = size_t{PAGE_COUNT} * sizeof(Entry);

struct PageTable::HostIndex {
  static constexpr u32 SLOT_BITS = 15, SLOTS = 1u << SLOT_BITS, NONE = 0xFFFFFFFFu;
  static constexpr u32 TRACKED = 0x10000000u >> PAGE_SHIFT;   // guest pages below 256 MB
  u64 key[SLOTS];      // host page number, 0 = empty (host pages are never at address 0)
  u32 head[SLOTS];
  u32 next[TRACKED], prev[TRACKED];   // doubly linked: removal is O(1) even for pages mirrored thousands of times
  HostIndex() { std::memset(key, 0, sizeof key); std::memset(head, 0xFF, sizeof head); std::memset(next, 0xFF, sizeof next); std::memset(prev, 0xFF, sizeof prev); }
  u32 slot(u64 hp) const {
    u32 s = static_cast<u32>((hp * 0x9E3779B97F4A7C15ull) >> (64 - SLOT_BITS));
    while (key[s] != 0 && key[s] != hp) s = (s + 1) & (SLOTS - 1);
    return s;
  }
};

PageTable::PageTable() {
  void* p = mmap(nullptr, TABLE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(p != MAP_FAILED);
  table_ = static_cast<Entry*>(p);   // zero-filled: every page starts unmapped
  index_ = new HostIndex;
}


static Entry make_entry(u32 guest_page_addr, u8* host, u32 flags) {
  if (flags & PAGE_MMIO) return TAG_SPECIAL;
  auto h = reinterpret_cast<u64>(host);
  assert((h & 3) == 0 && "host backing must be 4-byte aligned");
  u64 biased = h - guest_page_addr;          // wraps; recovered by (e<<2)+addr
  assert(((biased >> 2) & ~BASE_MASK) == 0 && "host pointer does not fit in 62 bits");
  Entry e = biased >> 2;
  if (!(flags & PAGE_WRITABLE)) e |= TAG_SPECIAL;
  if (PageTable::code_query && PageTable::code_query(host)) e |= TAG_CODE;
  return e;
}

void PageTable::map(u32 guest, u32 size, u8* host, u32 flags) {
  assert((guest % PAGE_SIZE) == 0 && (size % PAGE_SIZE) == 0);
  for (u32 off = 0; off < size; off += PAGE_SIZE) {
    u32 g = guest + off;
    const u32 p = g >> PAGE_SHIFT;
    const Entry e = make_entry(g, host + off, flags);
    if (table_[p] == e) continue;
    index_remove(p, table_[p]);
    table_[p] = e;
    index_insert(p, e);
  }
}

void PageTable::remap(u32 guest, u32 size, u8* const* hosts, u32 flags) {
  assert((guest % PAGE_SIZE) == 0 && (size % PAGE_SIZE) == 0 && !(flags & PAGE_MMIO));
  const bool writable = flags & PAGE_WRITABLE;
  for (u32 off = 0, i = 0; off < size; off += PAGE_SIZE, ++i) {
    const u32 g = guest + off;
    const u32 p = g >> PAGE_SHIFT;
    const Entry old = table_[p];
    u8* host = hosts[i];
    if (!host) {
      if (old) { index_remove(p, old); table_[p] = 0; }
      continue;
    }
    if (old && !(old & TAG_SPECIAL) == writable) {
      // Same backing as before: keep the entry (and its code tag).
      const u8* old_host = reinterpret_cast<const u8*>(((old & BASE_MASK) << 2) + g);
      if (old_host == host) continue;
    }
    const Entry e = make_entry(g, host, flags);
    if (old == e) continue;
    index_remove(p, old);
    table_[p] = e;
    index_insert(p, e);
  }
}

void PageTable::map_mmio(u32 guest, u32 size) {
  map(guest, size, nullptr, PAGE_MMIO);
}

void PageTable::unmap(u32 guest, u32 size) {
  assert((guest % PAGE_SIZE) == 0 && (size % PAGE_SIZE) == 0);
  for (u32 off = 0; off < size; off += PAGE_SIZE) {
    const u32 p = (guest + off) >> PAGE_SHIFT;
    if (!table_[p]) continue;
    index_remove(p, table_[p]);
    table_[p] = 0;
  }
}

PageTable::~PageTable() { munmap(table_, TABLE_BYTES); delete index_; }

static inline u64 host_page_of(Entry e, u32 p) { return ((e << 2) + (static_cast<u64>(p) << PAGE_SHIFT)) >> PAGE_SHIFT; }

void PageTable::index_insert(u32 p, Entry e) {
  if (p >= HostIndex::TRACKED || !(e << 2)) return;
  HostIndex& ix = *index_;
  const u64 hp = host_page_of(e, p);
  const u32 s = ix.slot(hp);
  ix.key[s] = hp;
  ix.next[p] = ix.head[s];
  ix.prev[p] = HostIndex::NONE;
  if (ix.head[s] != HostIndex::NONE) ix.prev[ix.head[s]] = p;
  ix.head[s] = p;
}

void PageTable::index_remove(u32 p, Entry e) {
  if (p >= HostIndex::TRACKED || !(e << 2)) return;
  HostIndex& ix = *index_;
  const u32 s = ix.slot(host_page_of(e, p));
  if (ix.key[s] == 0) return;
  const u32 n = ix.next[p], q = ix.prev[p];
  if (q != HostIndex::NONE) ix.next[q] = n; else if (ix.head[s] == p) ix.head[s] = n;
  if (n != HostIndex::NONE) ix.prev[n] = q;
  ix.next[p] = HostIndex::NONE; ix.prev[p] = HostIndex::NONE;
}

void PageTable::set_code_host(const u8* host_page, bool is_code) {
  const u64 want = reinterpret_cast<u64>(host_page) >> PAGE_SHIFT;
  const HostIndex& ix = *index_;
  const u32 s = ix.slot(want);
  if (ix.key[s] != want) return;
  for (u32 p = ix.head[s]; p != HostIndex::NONE; p = ix.next[p]) {
    Entry e = table_[p];
    table_[p] = is_code ? (e | TAG_CODE) : (e & ~TAG_CODE);
  }
}

void PageTable::set_code(u32 guest, u32 size, bool is_code) {
  u32 first = guest >> PAGE_SHIFT;
  u32 last  = (guest + size - 1) >> PAGE_SHIFT;
  for (u32 p = first; p <= last; ++p) {
    if (is_code) table_[p] |= TAG_CODE;
    else         table_[p] &= ~TAG_CODE;
  }
}

} // namespace ds::mem
