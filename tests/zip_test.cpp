// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Reading a ROM out of a zip, against archives built here so that every case
// -- including the malformed ones -- can be produced on purpose. This is the
// component where a bug hands back a silently corrupt ROM rather than an
// error, so the failure cases carry as much weight as the happy ones.
#include "core/cart/cart.h"
#include "core/cart/zip.h"
#include "check.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ds;

namespace {

// A game code that is in save_list.inc, and one that is not. The selection
// rule turns on exactly this distinction, so the test asserts the premise
// rather than trusting it -- a database regeneration that dropped the code
// would otherwise silently turn those cases into no-ops.
constexpr u32 kKnownCode   = 0x41464141;   // first entry of save_list.inc
constexpr u32 kUnknownCode = 0x5A5A5A5A;   // "ZZZZ"

// 96 KB of text, matching what tests/zip_deflate_blob.inc was compressed
// from. Regenerate both together:
//   data = b"".join(b"DSperate deflate window test block %06d\n" % i
//                   for i in range(4000))[:96*1024]
//   comp = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw, no zlib wrapper
std::vector<u8> window_text() {
  std::vector<u8> v;
  char line[64];
  for (int i = 0; v.size() < 96u * 1024u; ++i) {
    const int n = std::snprintf(line, sizeof line, "DSperate deflate window test block %06d\n", i);
    v.insert(v.end(), line, line + n);
  }
  v.resize(96 * 1024);
  return v;
}

#include "zip_deflate_blob.inc"

// A synthetic ROM: a real 0x160 header with the fields the picker reads, then
// filler so the image is a plausible size.
std::vector<u8> make_rom(u32 game_code, u8 revision, size_t size = 0x400, u8 fill = 0xA5) {
  std::vector<u8> rom(size, fill);
  std::memset(rom.data(), 0, sizeof(cart::Header));
  std::memcpy(rom.data(), "TESTROM     ", 12);
  std::memcpy(rom.data() + 12, &game_code, 4);
  rom[30] = revision;                       // Header::rom_version
  return rom;
}

// Builds archives byte by byte, so a field can be made wrong on purpose.
struct Zip {
  std::vector<u8> buf;
  struct Rec { std::string name; u16 method, flags; u32 csize, usize, local; };
  std::vector<Rec> recs;

  void u16le(u32 v) { buf.push_back(u8(v)); buf.push_back(u8(v >> 8)); }
  void u32le(u32 v) { for (int i = 0; i < 4; ++i) buf.push_back(u8(v >> (8 * i))); }
  void raw(const u8* p, size_t n) { buf.insert(buf.end(), p, p + n); }

  // DEFLATE's uncompressed block form: BFINAL|BTYPE=00, LEN, ~LEN, bytes.
  // A genuine method-8 stream that needs no compressor to produce, so the
  // container tests do not depend on the Huffman blob.
  static std::vector<u8> stored_blocks(const std::vector<u8>& in) {
    std::vector<u8> out;
    size_t off = 0;
    do {
      const size_t n = in.size() - off < 0xFFFF ? in.size() - off : 0xFFFF;
      const bool last = off + n == in.size();
      out.push_back(last ? 1 : 0);
      out.push_back(u8(n)); out.push_back(u8(n >> 8));
      out.push_back(u8(~n)); out.push_back(u8(~n >> 8));
      out.insert(out.end(), in.begin() + long(off), in.begin() + long(off + n));
      off += n;
    } while (off < in.size());
    return out;
  }

  // `usize_override` lets a test declare a size the stream does not produce.
  void add(const std::string& name, const std::vector<u8>& data, u16 method,
           u16 flags = 0, long usize_override = -1, const std::vector<u8>* payload = nullptr) {
    std::vector<u8> body = payload ? *payload
                         : method == 8 ? stored_blocks(data) : data;
    Rec r{name, method, flags, u32(body.size()),
          u32(usize_override >= 0 ? usize_override : long(data.size())), u32(buf.size())};
    u32le(0x04034B50); u16le(20); u16le(flags); u16le(method);
    u16le(0); u16le(0); u32le(0);
    u32le(r.csize); u32le(r.usize);
    u16le(u32(name.size())); u16le(0);
    raw(reinterpret_cast<const u8*>(name.data()), name.size());
    raw(body.data(), body.size());
    recs.push_back(r);
  }

  // Finishes the archive. `break_central` corrupts the first central-directory
  // signature; `drop_eocd` leaves the record off entirely.
  void finish(bool break_central = false, bool drop_eocd = false) {
    const u32 cd_off = u32(buf.size());
    for (const Rec& r : recs) {
      u32le(break_central && &r == &recs[0] ? 0xDEADBEEF : 0x02014B50);
      u16le(20); u16le(20); u16le(r.flags); u16le(r.method);
      u16le(0); u16le(0); u32le(0);
      u32le(r.csize); u32le(r.usize);
      u16le(u32(r.name.size())); u16le(0); u16le(0);
      u16le(0); u16le(0); u32le(0);
      u32le(r.local);
      raw(reinterpret_cast<const u8*>(r.name.data()), r.name.size());
    }
    const u32 cd_size = u32(buf.size()) - cd_off;
    if (drop_eocd) return;
    u32le(0x06054B50); u16le(0); u16le(0);
    u16le(u32(recs.size())); u16le(u32(recs.size()));
    u32le(cd_size); u32le(cd_off); u16le(0);
  }

  bool extract(std::vector<u8>& out, std::string& err, std::string* chosen = nullptr) const {
    return cart::extract_nds(buf.data(), buf.size(), out, err, chosen);
  }
};

void test_sniffing() {
  const u8 pk[] = {'P', 'K', 3, 4, 0};
  const u8 nds[] = {0x12, 0x34, 0, 0xEA};
  CHECK(cart::is_zip(pk, sizeof pk));
  CHECK(!cart::is_zip(nds, sizeof nds));
  CHECK(!cart::is_zip(pk, 3));            // too short to tell
  CHECK(!cart::is_zip(nullptr, 0));
}

// Both storage methods must reproduce the ROM byte for byte.
void test_roundtrip() {
  for (u16 method : {u16(0), u16(8)}) {
    const std::vector<u8> rom = make_rom(kKnownCode, 0, 0x4000, 0x5C);
    Zip z;
    z.add("game.nds", rom, method);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(err.empty());
    CHECK(out == rom);
    CHECK(chosen == "game.nds");
  }
}

// Dynamic Huffman codes and back-references reaching across the 32 KB
// dictionary window -- the streaming loop this project wrote.
void test_deflate_window() {
  const std::vector<u8> text = window_text();
  const std::vector<u8> comp(kDeflated, kDeflated + sizeof kDeflated);
  Zip z;
  z.add("big.nds", text, 8, 0, -1, &comp);
  z.finish();
  std::vector<u8> out; std::string err;
  CHECK(z.extract(out, err));
  CHECK(out.size() == text.size());
  CHECK(out == text);
}

// A name is matched case-insensitively, and non-.nds entries are ignored.
void test_entry_matching() {
  Zip z;
  z.add("readme.txt", std::vector<u8>(100, 'x'), 0);
  z.add("GAME.NDS", make_rom(kKnownCode, 0), 0);
  z.finish();
  std::vector<u8> out; std::string err, chosen;
  CHECK(z.extract(out, err, &chosen));
  CHECK(chosen == "GAME.NDS");
}

// The selection rule: database membership first, then the highest revision,
// then archive order.
void test_selection() {
  {   // a listed game code beats an unlisted one, whatever the order
    Zip z;
    z.add("hack.nds", make_rom(kUnknownCode, 9), 0);
    z.add("retail.nds", make_rom(kKnownCode, 0), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "retail.nds");   // beats revision 9: the database wins first
  }
  {   // among listed codes, the latest revision
    Zip z;
    z.add("rev0.nds", make_rom(kKnownCode, 0), 0);
    z.add("rev2.nds", make_rom(kKnownCode, 2), 0);
    z.add("rev1.nds", make_rom(kKnownCode, 1), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "rev2.nds");
  }
  {   // a tie falls back to archive order
    Zip z;
    z.add("first.nds", make_rom(kKnownCode, 3, 0x400, 0x11), 0);
    z.add("second.nds", make_rom(kKnownCode, 3, 0x400, 0x22), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "first.nds");
    CHECK(out[0x300] == 0x11);       // really the first one's bytes
  }
  {   // nothing in the database: the same rule with that term dropped
    Zip z;
    z.add("a.nds", make_rom(kUnknownCode, 1), 0);
    z.add("b.nds", make_rom(kUnknownCode, 3), 0);
    z.add("c.nds", make_rom(kUnknownCode, 2), 0);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "b.nds");
  }
  {   // the picker must read each candidate's header through its own
      // compression, not just the first entry's
    Zip z;
    z.add("old.nds", make_rom(kKnownCode, 1), 8);
    z.add("new.nds", make_rom(kKnownCode, 5), 8);
    z.finish();
    std::vector<u8> out; std::string err, chosen;
    CHECK(z.extract(out, err, &chosen));
    CHECK(chosen == "new.nds");
  }
}

// Every one of these must fail, and must not leave a partial image behind.
void test_failures() {
  // `expect` is a substring of the reason. Asserting only "it failed" is too
  // weak: several of these would still fail for an unrelated reason if the
  // check they are aimed at were removed, so the test would not notice.
  auto fails = [](const Zip& z, const char* expect) {
    std::vector<u8> out(999, 0xFF);
    std::string err;
    CHECK(!z.extract(out, err, nullptr));
    CHECK(err.find(expect) != std::string::npos);
    CHECK(out.empty() || out.size() == 999);   // never a half-written ROM
  };
  {   // no ROM in the archive
    Zip z; z.add("notes.txt", std::vector<u8>(50, 'q'), 0); z.finish();
    fails(z, "no .nds file");
  }
  {   // an encrypted entry is refused, not silently decompressed as garbage
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0, 1); z.finish();
    fails(z, "encrypted");
  }
  {   // a compression method we do not implement
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 12); z.finish();
    fails(z, "unsupported compression method");
  }
  {   // smaller than a DS header
    Zip z; z.add("game.nds", std::vector<u8>(0x40, 0), 0); z.finish();
    fails(z, "too small to be a ROM");
  }
  {   // truncated: the declared size is larger than the stream produces
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 8, 0, 0x8000); z.finish();
    fails(z, "ended early");
  }
  {   // stored, but the two sizes disagree
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 0, 0, 0x800); z.finish();
    fails(z, "ended early");
  }
  {   // an absurd declared size must be refused before anything is allocated
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0, 0x400), 8, 0, 0x7FFFFFFF); z.finish();
    fails(z, "larger than any DS card");
  }
  {   // corrupt central directory
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0); z.finish(true);
    fails(z, "bad central directory entry");
  }
  {   // no end-of-central-directory record
    Zip z; z.add("game.nds", make_rom(kKnownCode, 0), 0); z.finish(false, true);
    fails(z, "no end-of-central-directory");
  }
  {   // garbage that happens to start with the magic
    Zip z;
    z.buf = {'P', 'K', 3, 4, 0, 0, 0, 0, 0, 0};
    fails(z, "");
  }
  {   // a deflate stream of pure noise
    Zip z;
    std::vector<u8> junk(500);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = u8(i * 37 + 11);
    z.add("game.nds", std::vector<u8>(0x400, 0), 8, 0, 0x400, &junk);
    z.finish();
    fails(z, "ended early");
  }
}

// An offset that points outside the buffer must be caught, not followed.
void test_out_of_range() {
  Zip z;
  z.add("game.nds", make_rom(kKnownCode, 0), 0);
  z.finish();
  // Point the central directory's local-header offset past the end. It sits
  // at the last 4 bytes before the name in the central record.
  std::vector<u8> bad = z.buf;
  const size_t eocd = bad.size() - 22;
  const u32 cd_off = u32(bad[eocd + 16]) | (u32(bad[eocd + 17]) << 8) |
                     (u32(bad[eocd + 18]) << 16) | (u32(bad[eocd + 19]) << 24);
  for (int i = 0; i < 4; ++i) bad[cd_off + 42 + size_t(i)] = 0xFF;
  std::vector<u8> out; std::string err;
  CHECK(!cart::extract_nds(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());

  // A local header that is present and well-signed, but whose extra-field
  // length pushes the payload past the end of the buffer. This reaches the
  // second bounds check in data_offset(); the case above stops at the first.
  bad = z.buf;
  bad[26 + 2] = 0xFF; bad[26 + 3] = 0xFF;   // local header extra_len = 65535
  out.clear(); err.clear();
  CHECK(!cart::extract_nds(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());

  // And one whose compressed size claims more bytes than remain after it.
  bad = z.buf;
  for (int i = 0; i < 4; ++i) bad[18 + size_t(i)] = 0xFF;   // local csize
  const size_t eocd2 = bad.size() - 22;
  const u32 cd2 = u32(bad[eocd2 + 16]) | (u32(bad[eocd2 + 17]) << 8) |
                  (u32(bad[eocd2 + 18]) << 16) | (u32(bad[eocd2 + 19]) << 24);
  for (int i = 0; i < 4; ++i) bad[cd2 + 20 + size_t(i)] = 0xFE;   // central csize, not zip64
  out.clear(); err.clear();
  CHECK(!cart::extract_nds(bad.data(), bad.size(), out, err));
  CHECK(!err.empty());
}

// The premise the selection rule rests on.
void test_database_premise() {
  CHECK(cart::known_game_code(kKnownCode));
  CHECK(!cart::known_game_code(kUnknownCode));
}

} // namespace

int main() {
  test_database_premise();
  test_sniffing();
  test_roundtrip();
  test_deflate_window();
  test_entry_matching();
  test_selection();
  test_failures();
  test_out_of_range();
  std::printf("zip tests passed\n");
  return 0;
}
