// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.

#include "core/cart/zip.h"

#include <cstring>

#include "core/cart/cart.h"
#include "core/cart/miniz/miniz_tinfl.h"

namespace ds::cart {
namespace {

constexpr u32 SIG_LOCAL   = 0x04034B50;   // "PK\3\4"
constexpr u32 SIG_CENTRAL = 0x02014B50;   // "PK\1\2"
constexpr u32 SIG_EOCD    = 0x06054B50;   // "PK\5\6"

constexpr u16 METHOD_STORE   = 0;
constexpr u16 METHOD_DEFLATE = 8;

// The largest image we will unpack. A DS card tops out at 512 MB, so this is
// generous; the point is that the uncompressed size is a number an archive
// declares about itself, and without a bound a hostile or corrupt one asks us
// to allocate up to 4 GB before a single byte is decompressed.
constexpr u64 MAX_ROM = 512ull << 20;

// Every multi-byte field in a zip is little-endian and unaligned. These are
// the only way this file touches the buffer, and each one is bounds-checked
// by its caller before it is reached.
u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                               (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

bool ends_with_nds(const char* name, size_t len) {
  if (len < 4) return false;
  const char* e = name + len - 4;
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  return e[0] == '.' && lower(e[1]) == 'n' && lower(e[2]) == 'd' && lower(e[3]) == 's';
}

// One entry worth considering.
struct Entry {
  size_t   name_off = 0, name_len = 0;
  u16      method = 0;
  u64      csize = 0, usize = 0;
  size_t   local_off = 0;
  size_t   order = 0;         // position in the central directory, the tie-break
};

// Inflates raw DEFLATE from `src` into `dst`, stopping once `want` bytes have
// been produced. Used twice: once with want = 0x160 to peek at a candidate's
// header without paying for the whole ROM, and once with want = the declared
// uncompressed size to extract the winner. Returns the number of bytes
// produced, which the caller compares against what it asked for -- a stream
// that ends early is a corrupt archive, not a short ROM.
//
// The dictionary has to be the full 32 KB window whether or not we intend to
// keep all of it: a back-reference may reach that far, so decoding even the
// first 0x160 bytes correctly needs the real window.
size_t inflate_raw(const u8* src, size_t csize, u8* dst, size_t want) {
  tinfl_decompressor d;
  tinfl_init(&d);
  std::vector<u8> dict(TINFL_LZ_DICT_SIZE);
  size_t in_ofs = 0, dict_ofs = 0, out_total = 0;
  for (;;) {
    size_t in_bytes = csize - in_ofs;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
    const tinfl_status st = tinfl_decompress(&d, src + in_ofs, &in_bytes,
                                             dict.data(), dict.data() + dict_ofs, &out_bytes,
                                             TINFL_FLAG_HAS_MORE_INPUT);
    in_ofs += in_bytes;
    if (out_bytes) {
      const size_t take = out_total + out_bytes > want ? want - out_total : out_bytes;
      std::memcpy(dst + out_total, dict.data() + dict_ofs, take);
      out_total += take;
      if (out_total >= want) return out_total;   // got what we came for
    }
    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    if (st == TINFL_STATUS_DONE) return out_total;
    // NEEDS_MORE_INPUT with nothing left is a truncated stream; anything
    // negative is a malformed one. Both stop here and fail the size check.
    if (st != TINFL_STATUS_HAS_MORE_OUTPUT && st != TINFL_STATUS_NEEDS_MORE_INPUT) return out_total;
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_ofs >= csize) return out_total;
  }
}

// Copies `want` bytes of an entry's payload into `dst`, whichever way it was
// stored. False when the stream did not produce them.
bool read_entry(const u8* zip, const Entry& e, size_t data_off, u8* dst, size_t want) {
  if (want > e.usize) return false;
  if (e.method == METHOD_STORE) {
    if (e.csize != e.usize) return false;
    std::memcpy(dst, zip + data_off, want);
    return true;
  }
  return inflate_raw(zip + data_off, static_cast<size_t>(e.csize), dst, want) == want;
}

// Where an entry's payload starts. The central directory records the local
// header's offset, but the name and extra-field lengths there may differ from
// the local header's own, so the local header has to be read to find the
// payload. Returns false if it is not there or does not fit.
bool data_offset(const u8* zip, size_t size, const Entry& e, size_t& out) {
  if (e.local_off + 30 > size) return false;
  const u8* lh = zip + e.local_off;
  if (rd32(lh) != SIG_LOCAL) return false;
  const size_t off = e.local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (off > size || e.csize > size - off) return false;
  out = off;
  return true;
}

} // namespace

bool is_zip(const u8* data, size_t size) {
  return size >= 4 && rd32(data) == SIG_LOCAL;
}

bool extract_nds(const u8* zip, size_t size, std::vector<u8>& out, std::string& err,
                 std::string* chosen) {
  err.clear();
  // The end-of-central-directory record is last, but a trailing comment of up
  // to 64 KB may follow it, so it is found by scanning back over that window.
  if (size < 22) { err = "not a zip archive (too short)"; return false; }
  size_t eocd = 0;
  bool found = false;
  const size_t limit = size < 22 + 0xFFFF ? size : 22 + 0xFFFF;
  for (size_t back = 22; back <= limit; ++back) {
    if (rd32(zip + size - back) == SIG_EOCD) { eocd = size - back; found = true; break; }
  }
  if (!found) { err = "not a zip archive (no end-of-central-directory record)"; return false; }

  const u32 count = rd16(zip + eocd + 10);
  const u32 cd_size = rd32(zip + eocd + 12);
  const u32 cd_off = rd32(zip + eocd + 16);
  if (cd_off > size || cd_size > size - cd_off) { err = "corrupt zip (central directory out of range)"; return false; }

  std::vector<Entry> cands;
  size_t p = cd_off;
  const size_t cd_end = cd_off + cd_size;
  size_t nds_seen = 0, skipped_zip64 = 0, skipped_crypt = 0, skipped_method = 0, skipped_huge = 0;
  for (u32 i = 0; i < count && p + 46 <= cd_end; ++i) {
    const u8* h = zip + p;
    if (rd32(h) != SIG_CENTRAL) { err = "corrupt zip (bad central directory entry)"; return false; }
    const u16 flags = rd16(h + 8);
    const u16 method = rd16(h + 10);
    const u32 csize = rd32(h + 20), usize = rd32(h + 24);
    const u16 name_len = rd16(h + 28), extra_len = rd16(h + 30), cmt_len = rd16(h + 32);
    const u32 local_off = rd32(h + 42);
    const size_t name_off = p + 46;
    const size_t next = name_off + name_len + extra_len + cmt_len;
    if (next > cd_end) { err = "corrupt zip (central directory entry overruns)"; return false; }
    p = next;

    if (!ends_with_nds(reinterpret_cast<const char*>(zip + name_off), name_len)) continue;
    ++nds_seen;
    // Reasons an .nds entry cannot be used. Counted rather than fatal: a zip
    // may hold one usable ROM beside something we cannot read.
    if (flags & 1) { ++skipped_crypt; continue; }
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local_off == 0xFFFFFFFFu) { ++skipped_zip64; continue; }
    if (method != METHOD_STORE && method != METHOD_DEFLATE) { ++skipped_method; continue; }
    if (usize < 0x160) continue;      // smaller than a DS header: not a ROM
    if (usize > MAX_ROM) { ++skipped_huge; continue; }

    Entry e;
    e.name_off = name_off; e.name_len = name_len;
    e.method = method; e.csize = csize; e.usize = usize;
    e.local_off = local_off; e.order = cands.size();
    cands.push_back(e);
  }

  if (cands.empty()) {
    if (!nds_seen) err = "no .nds file in the archive";
    else if (skipped_crypt) err = "the .nds file in the archive is encrypted";
    else if (skipped_zip64) err = "the archive is zip64, which is not supported";
    else if (skipped_method) err = "the .nds file uses an unsupported compression method";
    else if (skipped_huge) err = "the .nds file in the archive is larger than any DS card";
    else err = "the .nds file in the archive is too small to be a ROM";
    return false;
  }

  // Pick. One candidate needs no header peek at all -- the common case, and
  // the expensive one to get wrong, since peeking inflates from the start of
  // the stream.
  size_t best = 0;
  if (cands.size() > 1) {
    int best_known = -1, best_rev = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
      size_t off = 0;
      if (!data_offset(zip, size, cands[i], off)) continue;
      u8 head[sizeof(Header)];
      if (!read_entry(zip, cands[i], off, head, sizeof head)) continue;
      Header hdr;
      std::memcpy(&hdr, head, sizeof hdr);
      const int known = known_game_code(hdr.game_code_u32()) ? 1 : 0;
      const int rev = hdr.rom_version;
      // Database membership first, then the highest revision, then archive
      // order -- which is why this is a strict > and never replaces on a tie.
      if (known > best_known || (known == best_known && rev > best_rev)) {
        best_known = known; best_rev = rev; best = i;
      }
    }
    if (best_known < 0) { err = "no readable .nds file in the archive"; return false; }
  }

  const Entry& e = cands[best];
  size_t off = 0;
  if (!data_offset(zip, size, e, off)) { err = "corrupt zip (entry data out of range)"; return false; }
  out.assign(static_cast<size_t>(e.usize), 0);
  if (!read_entry(zip, e, off, out.data(), out.size())) {
    out.clear();
    err = "corrupt zip (the compressed stream ended early)";
    return false;
  }
  if (chosen) chosen->assign(reinterpret_cast<const char*>(zip + e.name_off), e.name_len);
  return true;
}

} // namespace ds::cart
