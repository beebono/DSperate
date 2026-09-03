// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A zipped ROM as a RomSource without holding it in memory.
//
// A stored entry is mapped where it lies in the archive. A deflated one has
// no random access, so it is inflated once to a file and that file is
// mapped -- beside the archive, in `<dir>/.dsperate/<zip stem>.nds`, and kept:
// on the handhelds this exists for, writing 128 MB to the SD card takes
// twenty seconds and more, so the extraction is a one-time cost per game,
// not a per-launch one. The dot-directory keeps the game picker from listing
// the game twice. `/tmp` is never used: on those devices it is a tmpfs, i.e.
// the RAM the whole path is meant to save.
//
// A tag file beside the cached image records the archive's size and mtime,
// the entry's name, CRC and size; when any of them differs the image is
// rebuilt. The image is written to `.part` and renamed into place, so a
// power cut mid-extraction leaves nothing that could be mistaken for a ROM,
// and the CRC is checked as the bytes go out, so a corrupt archive fails
// loudly before anything is mapped (docs/cart-streaming-scoping.md).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/cart/rom_source.h"
#include "core/cart/zip.h"

namespace ds::cart {

struct ZipOpen {
  // Where to put the extracted image when the archive's own directory
  // cannot be written. Empty: fail instead.
  std::string fallback_dir;
  // The most the cache directory may hold, in bytes, before an extraction
  // evicts the least recently launched images to make room (0: no limit).
  // The image being opened is never evicted.
  u64 max_bytes = 0;
  // Called during an extraction with bytes done and total; null for none.
  ZipProgress progress = nullptr;
  void* progress_user = nullptr;
  // Set from another thread to abandon an extraction: open_zip fails with
  // "cancelled" and leaves nothing behind. Null: not cancellable.
  std::atomic<bool>* cancel = nullptr;
  // Filled in on success.
  std::string chosen;         // the entry's name
  std::string cache_path;     // the extracted image, empty when mapped in place
  bool extracted = false;     // true when the image was (re)built this time
};

// Opens `path` (an archive) as a RomSource. Null with a reason in `err`.
std::unique_ptr<RomSource> open_zip(const std::string& path, ZipOpen& how, std::string& err);

// Where open_zip would put (or find) the extracted image for `path`.
std::string zip_cache_path(const std::string& zip_path, const std::string& dir_override = {});

// The cache directory for archives in `dir` (its `.dsperate`).
std::string zip_cache_dir(const std::string& dir);

// What a cache directory holds. `stamp` is when the image was last launched
// (the tag's mtime; the image keeps its own for the validity check).
struct CacheEntry { std::string image, tag, archive; u64 bytes = 0; long stamp = 0; };
std::vector<CacheEntry> list_cache(const std::string& dir);

// Removes images whose archive no longer exists, images with no tag, and
// leftover `.part` files. Run by open_zip on every directory it touches, so
// a deleted or moved zip does not leave a full ROM behind. Returns bytes
// freed.
u64 sweep_cache(const std::string& dir);

// Removes every image in `dir` except `keep_image` (may be empty). The
// --clear-cache launch flag. Returns bytes freed.
u64 clear_cache(const std::string& dir, const std::string& keep_image);

// Removes one image and its tag (the session-mode cleanup on exit).
void remove_cached(const std::string& image);

} // namespace ds::cart
