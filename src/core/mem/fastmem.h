// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Guest memory mapped into host address space (DS_FASTMEM=1).
//
// The page table (page_table.h) stays the one source of truth: the
// interpreter, DMA, the scheduler and every slow path keep using it. What this
// adds is a second, derived way in for translated code: a per-CPU host address
// range `view` where `view + guest_address` is the guest byte, backed by the
// same physical pages as the table's buffers. A load or store through it is one
// host instruction, and anything the table would not serve directly -- I/O,
// VRAM, unmapped space, a store to a code page or a trapped page -- is not
// mapped there, so it faults instead, and the fault goes back to the table.
//
// HostArena: one shared-memory object holding every guest buffer, mapped
// once read-write (the canonical mapping the buffers point into). Created from
// memfd_create, else an unlinked file on a tmpfs directory; if neither can be
// had, there is no arena and no views, and the buffers come from
// aligned_alloc as before.
//
// GuestView: a PROT_NONE reservation per CPU -- the whole 4 GB guest space on
// a 64-bit host, the low 64 MB (the one region every direct access of the
// measured scenes falls in) on a 32-bit one. A 4 KB view page holds two 2 KB
// guest pages and may be mapped only when both halves are backed by the arena
// contiguously: read-write when both are plain RAM, read-only when both are
// readable, not at all otherwise. The invariant is one-sided: the view never
// serves an access the table would not; it may refuse ones the table would.
//
// Laid lazily, like melonDS's map-on-fault. The page table reports each entry
// it writes and flush() applies only the restrictions (unmap, or drop to
// read-only) -- those cannot wait, a page that has just become code must
// refuse stores before translated code runs again. Everything the table allows
// beyond what is laid is granted by fault(): an access the view refuses asks
// it first, and only an access the table refuses as well is a real refusal.
// Nothing is laid until it is touched, and a code page that an SMC cycle
// untags and tags again costs no system call at all while it stays read-only.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::mem {

// DS_FASTMEM: 1 = views are built (the arena and the table hook), 0 = off;
// unset = on where the JIT uses them (AArch64), off elsewhere.
bool fastmem_requested();

class HostArena {
public:
  // nullptr when no backing object can be made (the caller falls back).
  static std::unique_ptr<HostArena> create(size_t bytes);
  ~HostArena();
  HostArena(const HostArena&) = delete;
  HostArena& operator=(const HostArena&) = delete;

  // A zeroed slice, 4 KB-aligned in the object (and so PAGE_SIZE-aligned in
  // memory, which the JIT's code tracking needs). nullptr when full.
  u8* take(size_t bytes);
  int fd() const { return fd_; }
  u8* base() const { return base_; }
  size_t size() const { return size_; }
  const char* kind() const { return kind_; }
  bool contains(const u8* p) const { return p >= base_ && p < base_ + size_; }

private:
  HostArena() = default;
  int fd_ = -1;
  u8* base_ = nullptr;
  size_t size_ = 0, used_ = 0;
  const char* kind_ = "";
};

class GuestView {
public:
  static constexpr u32 HOST_PAGE = 4096;
#if UINTPTR_MAX > 0xFFFFFFFFu
  static constexpr u64 RESERVE = u64{1} << 32;    // the whole guest space: any address is either mapped or faults
  static constexpr u32 SPAN    = 0x10000000u;     // what is ever laid (the DS's memory lives below 256 MB)
#else
  static constexpr u64 RESERVE = u64{64} << 20;   // region 0x00000000-0x03FFFFFF
  static constexpr u32 SPAN    = 0x04000000u;
#endif

  static std::unique_ptr<GuestView> create(const HostArena& arena, const PageTable& table);
  ~GuestView();
  GuestView(const GuestView&) = delete;
  GuestView& operator=(const GuestView&) = delete;

  u8* base() const { return base_; }
  // The table wrote the entry of 2 KB guest page `guest_page`.
  void note(u32 guest_page) {
    const u32 v = guest_page >> 1;
    // VRAM is never laid (desired() refuses it), and its write traps toggle
    // hundreds of entries a frame: not worth a pending slot each.
    // A page with nothing laid has nothing to take away (fault() reads the
    // table afresh when it grants).
    if (v >= SPAN / HOST_PAGE || (v >> 12) == 0x6 || dirty_[v] || !laid_[v].off_plus1) return;
    dirty_[v] = 1;
    pending_.push_back(v);
  }
  void note_all();
  bool pending() const { return !pending_.empty(); }
  // Apply the restrictions the dirty pages need. Returns false (and logs) if a mapping call failed.
  bool flush();
  // A refused access at host address `addr` (from the fault handler; async-
  // signal-safe: table reads and mmap only). True when the table allows more
  // than the view had laid there -- the page (and what follows it that the
  // table allows too, up to a run) is laid now and the access can be retried.
  bool fault(uintptr_t addr);
  bool contains(uintptr_t addr) const { return addr >= reinterpret_cast<uintptr_t>(base_) && addr - reinterpret_cast<uintptr_t>(base_) < RESERVE; }
  // DS_FASTMEM_VERIFY: nothing laid is more than the table allows, and the
  // mapped pages' bytes are the table's own. Flushes first.
  bool verify(std::string* why);

  struct Stats { u64 flushes = 0, map_calls = 0, pages_laid = 0, flush_ns = 0, grants = 0; };
  const Stats& stats() const { return stats_; }

private:
  GuestView(const HostArena& a, const PageTable& t) : arena_(a), table_(t) {}
  struct Laid { u32 off_plus1 = 0; u8 writable = 0; bool operator==(const Laid& o) const { return off_plus1 == o.off_plus1 && writable == o.writable; } };
  Laid desired(u32 v) const;
  bool lay_run(u32 v0, u32 n, const Laid& first);
  bool protect_run(u32 v0, u32 n, bool writable);

  const HostArena& arena_;
  const PageTable& table_;
  u8* base_ = nullptr;
  std::vector<u8> dirty_;
  std::vector<u32> pending_;
  std::vector<Laid> laid_;
  Stats stats_;
};

} // namespace ds::mem
