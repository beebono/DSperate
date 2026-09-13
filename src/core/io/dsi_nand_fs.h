// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The emulator's own view of the DSi NAND filesystem (docs/dsiware-scoping.md
// 2.3): read the dump's files, and write into the session's in-memory
// sectors -- saves and settings in, a virtual title installed. The guest never
// goes through here; it reads raw sectors from the SD host.
//
// Two layers. FatVolume is FAT12/FAT16 over any byte-addressed device, so the
// same code reads the NAND's partitions, the FAT12 volume inside a title's
// public.sav, or an SD card image. NandFs is the NAND: the MBR, the AES-CTR
// every filesystem sector is encrypted with (melonDS DSi_NAND.cpp: the key
// from the console ID, the counter from the SHA-1 of the eMMC CID), and the
// ES encryption tickets carry. tools/dsi_nand.py is the independent reference
// for both.
#pragma once
#include "core/types.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include "core/crypto/aes.h"
}

namespace ds::io {

class NandImage;

class FatVolume {
 public:
  using ReadFn = std::function<void(u64 offset, u32 len, u8* out)>;
  using WriteFn = std::function<void(u64 offset, u32 len, const u8* in)>;

  struct Entry {
    std::string name;      // "PUBLIC.SAV", as stored (8.3, upper case)
    std::string long_name; // "TWLFontTable.dat" when a long-name entry precedes it, else empty
    u8  attr = 0;          // 0x10 directory
    u32 cluster = 0;       // first cluster; 0 for the root and for an empty file
    u32 size = 0;
    u64 dirent = 0;        // byte offset of the 32-byte entry (0 for the root)
    bool dir() const { return attr & 0x10; }
  };

  // `read`/`write` address the device in bytes from the start of the volume
  // (its boot sector) and are only ever called with whole sectors.
  bool open(ReadFn read, WriteFn write, std::string* err = nullptr);
  bool valid() const { return bps_ != 0; }
  int  fat_bits() const { return fat_bits_; }
  u32  cluster_bytes() const { return bps_ * spc_; }
  u32  free_clusters() const;

  // Paths are '/'-separated from the root and matched case-insensitively
  // against the 8.3 names and the long names. A file written under a name
  // that is not 8.3 gets a NAME~N.EXT short name and long-name entries, as
  // the DSi's /sys/TWLFontTable.dat has.
  bool lookup(const std::string& path, Entry& out) const;
  std::vector<Entry> list(const Entry& dir) const;
  bool read(const Entry& file, std::vector<u8>& out) const;

  // Replace a file's contents (existing file: its chain is grown or trimmed;
  // otherwise created in its parent directory, which must exist).
  bool write(const std::string& path, const u8* data, u32 len, std::string* err = nullptr);
  // Create a directory (its parent must exist); true if it already exists.
  bool mkdir(const std::string& path, std::string* err = nullptr);
  // Delete a file, or a directory with everything in it; true if absent.
  bool remove(const std::string& path, std::string* err = nullptr);

  // Every file and directory below the root, parents before children.
  void walk(const std::function<void(const std::string& path, const Entry&)>& fn) const;
  // Where a file lives: the volume-relative byte offset of each of its
  // clusters (cluster_bytes() long), for telling which files a session wrote.
  std::vector<u64> extents(const Entry& file) const;

 private:
  u32  fat_get(u32 c) const;
  void fat_set(u32 c, u32 v);
  void fat_flush();
  bool alloc_chain(u32 count, u32& first);
  void free_chain(u32 first);
  std::vector<u32> chain(u32 first) const;
  u64  cluster_offset(u32 c) const { return data_off_ + static_cast<u64>(c - 2) * bps_ * spc_; }
  std::vector<u8> dir_bytes(const Entry& dir) const;
  // Store `count` consecutive 32-byte entries (long-name entries, then the
  // short one); `dirent_out` is the last's offset.
  bool add_entry(const Entry& parent, const u8* raw, u32 count, u64& dirent_out);
  // The short name and long-name entries for `name` in `parent`: 1 entry when
  // it is already 8.3, else the long-name entries first.
  bool make_entries(const Entry& parent, const std::string& name, std::vector<u8>& raws) const;
  void write_entry(u64 dirent, const u8 raw[32]);
  bool split(const std::string& path, Entry& parent, std::string& name, std::string* err) const;
  static bool to_83(const std::string& name, u8 out[11]);

  ReadFn read_;
  WriteFn write_;
  u32 bps_ = 0, spc_ = 0, clusters_ = 0;
  int fat_bits_ = 0;
  u32 nfats_ = 0, fat_sectors_ = 0, root_entries_ = 0;
  u64 fat_off_ = 0, root_off_ = 0, data_off_ = 0;
  std::vector<u8> fat_;             // the first FAT, cached; written to every copy
  std::vector<bool> fat_dirty_;     // per FAT sector
};

class NandFs {
 public:
  // `bios7i` (64 KB) supplies the ES key's KeyY at 0x8308; without it tickets
  // cannot be made or read, and everything else still works.
  bool mount(NandImage& nand, const u8* bios7i, std::string* err = nullptr);
  // Write an empty DSi filesystem over `nand` (for a synthesised image, see
  // dsi_nand_synth.h) and mount it: the MBR and the two FAT16 partitions with
  // a retail DSi's geometry, encrypted under the image's console ID and CID.
  // The image must be at least kImageBytes long.
  static constexpr u64 kImageBytes = 0xF000000;
  bool format(NandImage& nand, const u8* bios7i, std::string* err = nullptr);
  bool valid() const { return main_.valid(); }

  FatVolume& main() { return main_; }     // partition 0: title/, ticket/, shared1/, sys/
  FatVolume& photo() { return photo_; }   // partition 1: the camera's photos (may be invalid)
  u64 main_base() const { return main_base_; }     // each partition's byte offset in the image
  u64 photo_base() const { return photo_base_; }

  // melonDS NANDImage::ESEncrypt/ESDecrypt. `data` holds `len` bytes followed
  // by a 0x20-byte MAC + footer area; decrypt returns false on a bad MAC.
  bool has_es_key() const { return es_key_ok_; }
  void es_encrypt(u8* data, u32 len, const u8 nonce[12]) const;
  bool es_decrypt(u8* data, u32 len) const;

 private:
  void setup_crypto(NandImage& nand, const u8* bios7i);
  void crypt_read(u64 offset, u32 len, u8* out);
  void crypt_write(u64 offset, u32 len, const u8* in);
  void xcrypt(u64 offset, u8* buf, u32 len) const;

  NandImage* nand_ = nullptr;
  AES_ctx fat_ctx_{};
  u8 fat_iv_[16] = {};              // counter base, big-endian
  u8 es_key_[16] = {};
  bool es_key_ok_ = false;
  FatVolume main_, photo_;
  u64 main_base_ = 0, photo_base_ = 0;
};

}  // namespace ds::io
