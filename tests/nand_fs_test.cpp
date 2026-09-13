// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi NAND filesystem layer (io/dsi_nand_fs): SHA-1 against the FIPS
// vectors, FatVolume on a FAT12 image built here (directories, files across
// clusters, overwrites that grow and shrink, a re-open from the raw bytes),
// and -- when DS_TEST_DSI_NAND and DS_TEST_DSI_BIOS7I name a dump and its
// ARM7 BIOS, which cannot ship with the tests -- the real NAND: mount, a
// title's save and the FAT12 inside it, the dump's tickets through ES
// decryption, and a write that stays in the session's memory. With
// DS_TEST_DSI_NAND_COPY naming a scratch copy as well, that write goes into
// the copy, for tools/dsi_nand.py to read back independently. The same variables
// run the save/system-file persistence round trip (io/dsi_nand_persist).
#include "core/crypto/sha1.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_sd.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

using namespace ds;
using ds::io::FatVolume;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::string hex(const u8* p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; }
  return s;
}

static void test_sha1() {
  u8 d[20];
  crypto::sha1(reinterpret_cast<const u8*>("abc"), 3, d);
  CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  crypto::sha1(nullptr, 0, d);
  CHECK(hex(d, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  // 56 bytes: the length no longer fits after the terminator, so two blocks.
  const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  crypto::sha1(reinterpret_cast<const u8*>(m), std::strlen(m), d);
  CHECK(hex(d, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// A FAT12 volume the shape of a small floppy: 512-byte sectors, one sector
// per cluster, 1 reserved sector, 2 FATs of 3 sectors, 112 root entries.
static std::vector<u8> make_fat12(u32 total_sectors) {
  std::vector<u8> img(static_cast<size_t>(total_sectors) * 512, 0);
  u8* b = img.data();
  b[0x0B] = 0x00; b[0x0C] = 0x02;   // 512 bytes per sector
  b[0x0D] = 1;                      // sectors per cluster
  b[0x0E] = 1;                      // reserved
  b[0x10] = 2;                      // FATs
  b[0x11] = 112;                    // root entries
  b[0x13] = static_cast<u8>(total_sectors); b[0x14] = static_cast<u8>(total_sectors >> 8);
  b[0x16] = 3;                      // sectors per FAT
  b[0x1FE] = 0x55; b[0x1FF] = 0xAA;
  for (int f = 0; f < 2; ++f) { u8* fat = b + 512 * (1 + 3 * f); fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; }
  return img;
}

static bool open_on(FatVolume& v, std::vector<u8>& img) {
  return v.open([&img](u64 o, u32 n, u8* out) { std::memcpy(out, img.data() + o, n); },
                [&img](u64 o, u32 n, const u8* in) { std::memcpy(img.data() + o, in, n); });
}

static void test_fat12() {
  std::vector<u8> img = make_fat12(1440);
  FatVolume v;
  CHECK(open_on(v, img));
  CHECK(v.fat_bits() == 12);
  const u32 free0 = v.free_clusters();

  std::string err;
  CHECK(v.mkdir("/title", &err));
  CHECK(v.mkdir("/TITLE/00030004", &err));   // case-insensitive
  CHECK(v.mkdir("/title/00030004", &err));   // already there: fine
  CHECK(!v.mkdir("/nope/deeper", &err));     // no parent

  std::vector<u8> data(5000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 7 + 3);
  CHECK(v.write("/title/00030004/public.sav", data.data(), static_cast<u32>(data.size()), &err));
  CHECK(v.free_clusters() == free0 - 2 - 10);   // two directory clusters, ten data clusters

  // Enough files to spill a subdirectory past its first cluster (16 entries).
  for (int i = 0; i < 20; ++i) {
    const std::string p = "/title/00030004/F" + std::to_string(i) + ".BIN";
    const u8 byte = static_cast<u8>(i);
    CHECK(v.write(p, &byte, 1, &err));
  }

  // Grow, then shrink, an existing file.
  std::vector<u8> big(9000, 0xAB);
  CHECK(v.write("/title/00030004/public.sav", big.data(), static_cast<u32>(big.size()), &err));
  std::vector<u8> small(100, 0xCD);
  CHECK(v.write("/title/00030004/public.sav", small.data(), static_cast<u32>(small.size()), &err));

  // Everything again from the raw bytes alone.
  FatVolume w;
  CHECK(open_on(w, img));
  FatVolume::Entry e;
  std::vector<u8> got;
  CHECK(w.lookup("/title/00030004/PUBLIC.SAV", e) && w.read(e, got) && got == small);
  CHECK(w.lookup("/title/00030004/F19.BIN", e) && w.read(e, got) && got.size() == 1 && got[0] == 19);
  CHECK(w.lookup("/title/00030004/F0.BIN", e) && w.read(e, got) && got.size() == 1 && got[0] == 0);
  int files = 0;
  w.walk([&](const std::string&, const FatVolume::Entry& x) { files += !x.dir(); });
  CHECK(files == 21);
  // 2 directory clusters + 1 more for the spilled directory, 1 for public.sav, 20 one-byte files.
  CHECK(w.free_clusters() == free0 - 3 - 1 - 20);
  CHECK(!w.write("/title/00030004/toolongname.sav", small.data(), 1, &err));
}

static std::vector<u8> slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

static void test_real_nand() {
  const char* path = std::getenv("DS_TEST_DSI_NAND");
  const char* b7path = std::getenv("DS_TEST_DSI_BIOS7I");
  if (!path || !b7path) { std::printf("nand_fs: DS_TEST_DSI_NAND / DS_TEST_DSI_BIOS7I not set, real-dump checks skipped\n"); return; }
  const std::vector<u8> bios7i = slurp(b7path);
  CHECK(bios7i.size() == 0x10000);
  if (bios7i.size() != 0x10000) return;

  const char* copy = std::getenv("DS_TEST_DSI_NAND_COPY");
  io::NandImage nand;
  CHECK(nand.open(copy ? copy : path, copy != nullptr));
  io::NandFs fs;
  std::string err;
  CHECK(fs.mount(nand, bios7i.data(), &err));
  if (!fs.valid()) { std::fprintf(stderr, "mount: %s\n", err.c_str()); return; }
  CHECK(fs.main().fat_bits() == 16);

  // A title's save, and the FAT12 volume the SDK formats inside it.
  FatVolume::Entry e;
  std::vector<u8> sav;
  CHECK(fs.main().lookup("/title/00030004/4b533345/data/public.sav", e) && fs.main().read(e, sav));
  CHECK(sav.size() == 16384);
  FatVolume inner;
  CHECK(inner.open([&sav](u64 o, u32 n, u8* out) { std::memcpy(out, sav.data() + o, n); },
                   [&sav](u64 o, u32 n, const u8* in) { std::memcpy(sav.data() + o, in, n); }, &err));
  CHECK(inner.fat_bits() == 12);

  // Every DSiWare ticket in the dump decrypts under the ES key with a good MAC.
  FatVolume::Entry tdir;
  int tickets = 0;
  if (fs.main().lookup("/ticket/00030004", tdir)) {
    for (const FatVolume::Entry& t : fs.main().list(tdir)) {
      if (t.dir()) continue;
      std::vector<u8> tik;
      CHECK(fs.main().read(t, tik) && tik.size() == 0x2C4);
      CHECK(fs.es_decrypt(tik.data(), 0x2A4));
      ++tickets;
    }
  }
  CHECK(tickets > 0);

  // A ticket the way melonDS's ImportTitle makes one survives the round trip.
  u8 tik[0x2C4] = {};
  tik[0x1DC] = 0x00; tik[0x1DD] = 0x03; tik[0x1DE] = 0x00; tik[0x1DF] = 0x04;
  const u8 nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  fs.es_encrypt(tik, 0x2A4, nonce);
  CHECK(fs.es_decrypt(tik, 0x2A4));
  CHECK(tik[0x1DD] == 0x03 && tik[0x1DF] == 0x04);

  // Write through the session: a directory and a file of several clusters.
  const u32 free0 = fs.main().free_clusters();
  std::vector<u8> data(3 * fs.main().cluster_bytes() + 77);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i ^ (i >> 9));
  CHECK(fs.main().mkdir("/dsperate", &err));
  CHECK(fs.main().write("/dsperate/probe.bin", data.data(), static_cast<u32>(data.size()), &err));
  CHECK(fs.main().free_clusters() == free0 - 1 - 4);
  if (!copy) CHECK(!nand.written_sectors().empty());

  io::NandFs again;
  CHECK(again.mount(nand, bios7i.data(), &err));
  std::vector<u8> back;
  CHECK(again.main().lookup("/DSPERATE/PROBE.BIN", e) && again.main().read(e, back) && back == data);
  CHECK(again.main().lookup("/title/00030004/4b533345/data/public.sav", e) && again.main().read(e, back) && back.size() == 16384);
  std::printf("nand_fs: real dump ok (%d tickets, %u clusters free before the probe)\n", tickets, free0);
}

// Saves and system files in and out of a session (io/dsi_nand_persist), on a
// fresh read-only session over the real dump: a host save goes in and is what
// the NAND then holds, an unchanged session exports nothing, a changed save
// and a changed system file come back out, and the dump is never written.
static void test_persist() {
  const char* path = std::getenv("DS_TEST_DSI_NAND");
  const char* b7path = std::getenv("DS_TEST_DSI_BIOS7I");
  if (!path || !b7path || std::getenv("DS_TEST_DSI_NAND_COPY")) return;
  const std::vector<u8> bios7i = slurp(b7path);
  if (bios7i.size() != 0x10000) return;
  namespace fsys = std::filesystem;
  const fsys::path dir = fsys::temp_directory_path() / ("dsperate_persist_test_" + std::to_string(::getpid()));
  fsys::remove_all(dir);
  fsys::create_directories(dir);
  const io::NandPersistPaths paths{dir.string(), (dir / "nand.ovr").string(), (dir / "photos").string()};

  std::vector<u8> save(16384);
  for (size_t i = 0; i < save.size(); ++i) save[i] = static_cast<u8>(i * 13 + 5);
  { std::ofstream f(dir / "KS3E.pub", std::ios::binary); f.write(reinterpret_cast<const char*>(save.data()), save.size()); }
  const u8 wrong[3] = {1, 2, 3};
  { std::ofstream f(dir / "KMGE.pub", std::ios::binary); f.write(reinterpret_cast<const char*>(wrong), 3); }

  io::NandImage nand;
  CHECK(nand.open(path));
  io::NandPersistReport in = io::nand_import(nand, bios7i.data(), paths);
  CHECK(in.saves == 1);                 // KS3E; KMGE's is the wrong size and is left alone
  CHECK(in.notes.size() == 1);

  io::NandFs fs;
  std::string err;
  CHECK(fs.mount(nand, bios7i.data(), &err));
  FatVolume::Entry e;
  std::vector<u8> got;
  CHECK(fs.main().lookup("/title/00030004/4b533345/data/public.sav", e) && fs.main().read(e, got) && got == save);

  // Nothing changed since the import: nothing to write.
  io::NandPersistReport out = io::nand_export(nand, bios7i.data(), paths);
  CHECK(out.saves == 0 && out.system_files == 0);

  // The session changes the save and a system file.
  save[100] ^= 0xFF;
  CHECK(fs.main().write("/title/00030004/4b533345/data/public.sav", save.data(), static_cast<u32>(save.size()), &err));
  CHECK(fs.main().lookup("/shared1/TWLCFG0.dat", e) && fs.main().read(e, got));
  got[0x100] ^= 0x5A;
  CHECK(fs.main().write("/shared1/TWLCFG0.dat", got.data(), static_cast<u32>(got.size()), &err));
  out = io::nand_export(nand, bios7i.data(), paths);
  CHECK(out.saves == 1 && out.system_files == 1);
  std::vector<u8> host = slurp((dir / "KS3E.pub").string().c_str());
  CHECK(host == save);

  // A new session over the untouched dump takes both back in.
  io::NandImage nand2;
  CHECK(nand2.open(path));
  in = io::nand_import(nand2, bios7i.data(), paths);
  CHECK(in.saves == 1 && in.system_files == 1);
  io::NandFs fs2;
  CHECK(fs2.mount(nand2, bios7i.data(), &err));
  std::vector<u8> cfg;
  CHECK(fs2.main().lookup("/shared1/TWLCFG0.dat", e) && fs2.main().read(e, cfg) && cfg == got);

  fsys::remove_all(dir);
  std::printf("nand_fs: persistence ok\n");
}

int main() {
  test_sha1();
  test_fat12();
  test_real_nand();
  test_persist();
  if (failures) { std::fprintf(stderr, "nand_fs: %d failure(s)\n", failures); return 1; }
  std::printf("nand_fs: ok\n");
  return 0;
}
