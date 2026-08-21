// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/page_table.h"

#include <cassert>
#include <cstring>
#include <sys/mman.h>

namespace ds::mem {

void (*code_write_hook)(u8* host, u32 len) = nullptr;
bool (*PageTable::code_query)(const u8* host_page) = nullptr;

static constexpr size_t TABLE_BYTES = size_t{PAGE_COUNT} * sizeof(Entry);

PageTable::PageTable() {
  void* p = mmap(nullptr, TABLE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(p != MAP_FAILED);
  table_ = static_cast<Entry*>(p);   // zero-filled: every page starts unmapped
}

PageTable::~PageTable() { munmap(table_, TABLE_BYTES); }

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
    table_[g >> PAGE_SHIFT] = make_entry(g, host + off, flags);
  }
}

void PageTable::map_mmio(u32 guest, u32 size) {
  map(guest, size, nullptr, PAGE_MMIO);
}

void PageTable::unmap(u32 guest, u32 size) {
  assert((guest % PAGE_SIZE) == 0 && (size % PAGE_SIZE) == 0);
  for (u32 off = 0; off < size; off += PAGE_SIZE)
    table_[(guest + off) >> PAGE_SHIFT] = 0;
}

void PageTable::set_code_host(const u8* host_page, bool is_code) {
  const u64 want = reinterpret_cast<u64>(host_page) >> PAGE_SHIFT;
  for (u32 p = 0; p < (0x10000000u >> PAGE_SHIFT); ++p) {
    Entry e = table_[p];
    if (!(e << 2)) continue;
    const u64 host = ((e << 2) + (static_cast<u64>(p) << PAGE_SHIFT)) >> PAGE_SHIFT;
    if (host != want) continue;
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
