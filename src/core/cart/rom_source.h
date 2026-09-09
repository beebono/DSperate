// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Where a cart's ROM bytes come from. The cart itself only ever wants one
// 4 KB page at a time (a block read wraps inside its page), a few small
// reads at construction, and one 2 KB window it may rewrite -- so this is the
// whole interface, and it lets the bytes live wherever is cheapest:
//
//   * mapped: a read-only private mmap of the file, or of a range of it (a
//     stored entry inside a zip). The kernel demand-pages from disk and the
//     page cache is reclaimable, so a 256 MB dump costs the process nothing it
//     has not touched. What the A30 needs (docs/cart-streaming-scoping.md).
//   * owned: a vector, for the frontend's built-in loader cart, tests, and
//     a zip that had to be inflated (until phase 2 puts that on disk).
//
// The two are indistinguishable from the cart's side, which is what the
// scene-hash gate checks. Two details that keep it exact:
//
//   * A real card wraps its address into a power of two and reads past the
//     end of the image return 0xFF. The vector used to be padded to make that
//     so; here pages at or beyond `size()` are a static 0xFF page, and the
//     partial last page of an unpadded file is served from a padded copy --
//     mmap zero-fills past EOF, and a fault beyond the last file page is
//     SIGBUS, so that page is never read through the mapping.
//   * The secure-area re-encryption at construction writes 0x800 bytes. A
//     mapped source is PROT_READ; `patch()` copies the page into an overlay
//     that `page()` serves first. Only that one page ever ends up there.
#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cart {

class RomSource {
public:
  static constexpr u32 PAGE = 0x1000;

  ~RomSource();
  RomSource(const RomSource&) = delete;
  RomSource& operator=(const RomSource&) = delete;

  static std::unique_ptr<RomSource> from_memory(std::vector<u8> bytes);
  // Maps `size` bytes of `path` at `offset` (0, 0 = the whole file). The
  // offset need not be page-aligned; the mapping starts at the page below
  // and the pointer is adjusted. False with a reason in `err`.
  static std::unique_ptr<RomSource> map_file(const std::string& path, u64 offset, u64 size,
                                             std::string& err);
  static std::unique_ptr<RomSource> map_file(const std::string& path, std::string& err) {
    return map_file(path, 0, 0, err);
  }

  u32 size() const { return size_; }          // bytes in the image
  u32 mask() const { return mask_; }          // padded power-of-two size minus one
  bool mapped() const { return map_ != nullptr; }

  // Pulls the whole mapping into the page cache on a worker thread. Opt-in
  // (DS_CART_PREFETCH=1), for A/B only: measured on the A30 it loses
  // everywhere -- a 270-350 ms hitch at frame 0 while the worker fights
  // startup for the SD bus, +4 ms at p90 on Golden Sun for the ten seconds
  // a 256 MB read takes -- while plain demand paging matches an in-RAM copy
  // warm, because the game's cart reads are 95 % sequential and the
  // kernel's readahead already serves them
  // (docs/cart-streaming-scoping.md, phase 4). Refuses when the system has
  // no room for it (MemAvailable below size + `margin`). Returns whether
  // it started.
  bool prefetch(u64 margin = 128ull << 20);
  bool prefetching() const { return worker_.joinable(); }

  // The 4 KB page holding `addr` (masked). Never null: past the image it is
  // the 0xFF page. The pointer is good for the life of the source.
  const u8* page(u32 addr) const {
    const u32 p = addr & mask_ & ~(PAGE - 1);
    if (!overlay_.empty()) {
      for (const Patch& o : overlay_) if (o.base == p) return o.bytes.data();
    }
    return page_unpatched(p);
  }
  // A writable copy of the page holding `addr`, served by page() from then
  // on. For the secure-area rewrite only.
  u8* patch(u32 addr);

  // Small bounded reads for construction and direct boot. Beyond the image
  // the bytes are 0xFF.
  void read(u32 addr, u8* dst, u32 n) const;
  // The same, but reading past any patch() overlay to the bytes the file
  // actually holds, and reporting how many of them there were: `n` minus the
  // part of the request that fell beyond the image (which is still filled
  // with 0xFF, as read() would).
  //
  // This exists for the RetroAchievements hash and wants care. That hash
  // covers the ARM9 binary, which begins at the secure area -- exactly the
  // 0x800 bytes Cart re-encrypts through patch(). Hashing through read()
  // would hash our rewrite rather than the file, and the symptom is not an
  // error but a hash RetroAchievements has never seen, i.e. a game that
  // silently has no achievements. The short count matters for the same
  // reason: rcheevos 0-pads a truncated icon block, so it has to be able to
  // tell a short read from 0xFF padding. See docs/retroachievements-scoping.md.
  u32 read_unpatched(u32 addr, u8* dst, u32 n) const;
  u32 read32(u32 addr) const { u8 b[4]; read(addr, b, 4); return static_cast<u32>(b[0]) | (b[1] << 8) | (b[2] << 16) | (static_cast<u32>(b[3]) << 24); }

private:
  RomSource() = default;
  void finish();   // mask_, tail_ from data_/size_
  static const u8* ff_page();
  // `p` already masked and page-aligned; the overlay is not consulted.
  const u8* page_unpatched(u32 p) const {
    if (p + PAGE <= size_) return data_ + p;
    if (p < size_) return tail_.data();
    return ff_page();
  }

  struct Patch { u32 base; std::vector<u8> bytes; };
  const u8* data_ = nullptr;
  u32 size_ = 0, mask_ = 0;
  std::vector<u8> owned_;
  std::vector<u8> tail_;
  std::vector<Patch> overlay_;
  void* map_ = nullptr; size_t map_len_ = 0;
  std::thread worker_;
  std::atomic<bool> stop_{false};
};

} // namespace ds::cart
