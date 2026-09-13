// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The emulator's own view of the DSi NAND filesystem (docs/dsiware-scoping.md
// 2.3): read the dump's files, and write into the session's in-memory
// sectors -- saves and settings in, a virtual title installed. The guest never
// goes through here; it reads raw sectors from the SD host.
//
// Two layers. FatVolume is FAT12/16/32 over any byte-addressed device, so the
// same code reads the NAND's partitions, the FAT12 volume inside a title's
// public.sav, or the SD card (dsi_sd_card.h). NandFs is the NAND: the MBR, the AES-CTR
// every filesystem sector is encrypted with (melonDS DSi_NAND.cpp: the key
// from the console ID, the counter from the SHA-1 of the eMMC CID), and the
// ES encryption tickets carry. tools/dsi_nand.py is the independent reference
// for both.
#pragma once
#include "core/types.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_set>
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
    std::string long_name; // "TWLFontTable.dat" (UTF-8) when a long-name entry precedes it, else empty
    u8  attr = 0;          // 0x10 directory
    u32 cluster = 0;       // first cluster; 0 for an empty file, and for the root except on FAT32
    u32 size = 0;
    u64 dirent = 0;        // byte offset of the 32-byte entry (0 for the root)
    u16 mdate = 0, mtime = 0;   // last modified, FAT encoding
    bool dir() const { return attr & 0x10; }
    const std::string& display_name() const { return long_name.empty() ? name : long_name; }
  };

  // A FAT timestamp for the entries a write creates or updates. Without one,
  // entries carry 2000-01-01 00:00, the date the DSi's own files have.
  struct Stamp { u16 date = 0, time = 0; };

  // `read`/`write` address the device in bytes from the start of the volume
  // (its boot sector) and are only ever called with whole sectors.
  bool open(ReadFn read, WriteFn write, std::string* err = nullptr);
  bool valid() const { return bps_ != 0; }
  int  fat_bits() const { return fat_bits_; }
  u32  cluster_bytes() const { return bps_ * spc_; }
  u32  free_clusters() const { return free_count_; }
  u32  cluster_count() const { return clusters_; }
  Entry root() const;

  // Write an empty volume through `write`: boot sector (plus FSInfo and the
  // backup boot sector on FAT32), FATs and root directory. The layout is
  // computed from the size; `fat_bits` must suit the cluster count it gives.
  // With `zeroed`, the device already reads zero and all-zero sectors are
  // skipped (an in-memory image stays sparse).
  struct FormatSpec {
    u64 sectors = 0;          // the volume's length in 512-byte sectors
    u32 hidden = 0;           // sectors before it (its partition's start)
    int fat_bits = 16;        // 16 or 32
    u32 sectors_per_cluster = 32;
    const char* oem = "DSPERATE";
    const char* label = "NO NAME";
    u32 serial = 0x12345678;
    bool zeroed = false;
  };
  static bool format(const WriteFn& write, const FormatSpec& spec, std::string* err = nullptr);

  // Names created on this volume keep their case: a name that is 8.3 apart
  // from lower-case letters gets long-name entries as well, as a PC writes
  // "readme.txt". Off for the NAND, whose files are all upper-case 8.3.
  void set_preserve_case(bool on) { preserve_case_ = on; }

  // Paths are '/'-separated from the root and matched case-insensitively
  // against the 8.3 names and the long names. A file written under a name
  // that is not 8.3 gets a NAME~N.EXT short name and long-name entries, as
  // the DSi's /sys/TWLFontTable.dat has.
  bool lookup(const std::string& path, Entry& out) const;
  std::vector<Entry> list(const Entry& dir) const;
  bool read(const Entry& file, std::vector<u8>& out) const;

  // Replace a file's contents (existing file: its chain is grown or trimmed;
  // otherwise created in its parent directory, which must exist).
  bool write(const std::string& path, const u8* data, u32 len, std::string* err = nullptr, const Stamp* stamp = nullptr);
  // The same without the data: the entry and a chain of `len` bytes, whose
  // clusters are left as the device has them. For contents that live
  // somewhere else (the SD card's host files); `out` is the new entry.
  bool create(const std::string& path, u32 len, Entry* out, std::string* err = nullptr, const Stamp* stamp = nullptr);
  // Create a directory (its parent must exist); true if it already exists.
  bool mkdir(const std::string& path, std::string* err = nullptr, const Stamp* stamp = nullptr);

  // Fill a directory with many new entries in one pass: a bulk build, where
  // create()/mkdir() would re-read the directory for every entry. Files get
  // a chain of `size` bytes with their clusters left as the device has them;
  // directories get one cluster holding "." and "..". An item that cannot be
  // made (name, space) is left with ok false and why set; the call fails
  // only when the entries do not fit the directory (a FAT16 root holds 512).
  struct NewEntry {
    std::string name;
    bool dir = false;
    u32 size = 0;
    Stamp stamp;
    bool ok = false;
    std::string why;
    Entry out;
  };
  bool populate(const Entry& dir, std::vector<NewEntry>& items, std::string* err = nullptr);
  // Clusters nothing has used read zero, as on a volume just formatted onto a
  // zeroed device: new directory clusters then skip their zero fill.
  void set_fresh(bool on) { fresh_ = on; }
  u64 data_offset() const { return data_off_; }   // cluster 2's byte offset in the volume
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
  u32  eoc_min() const { return fat_bits_ == 12 ? 0xFF8 : fat_bits_ == 16 ? 0xFFF8 : 0x0FFFFFF8; }
  u32  eoc_mark() const { return fat_bits_ == 12 ? 0xFFF : fat_bits_ == 16 ? 0xFFFF : 0x0FFFFFFF; }
  bool alloc_chain(u32 count, u32& first);
  void free_chain(u32 first);
  std::vector<u32> chain(u32 first) const;
  u64  cluster_offset(u32 c) const { return data_off_ + static_cast<u64>(c - 2) * bps_ * spc_; }
  u32  entry_cluster(const u8* e) const;
  void set_entry_cluster(u8* e, u32 c) const;
  std::vector<u8> dir_bytes(const Entry& dir) const;
  // Store `count` consecutive 32-byte entries (long-name entries, then the
  // short one); `dirent_out` is the last's offset.
  bool add_entry(const Entry& parent, const u8* raw, u32 count, u64& dirent_out);
  // The short name and long-name entries for `name` in `parent`: 1 entry when
  // it is already 8.3, else the long-name entries first.
  bool make_entries(const Entry& parent, const std::string& name, std::vector<u8>& raws, std::string* why) const;
  bool make_entries_in(const std::unordered_set<std::string>& taken, const std::string& name, std::vector<u8>& raws, std::string* why) const;
  void write_dir_cluster(u32 cluster, u32 parent_cluster, const Stamp* stamp);
  void write_entry(u64 dirent, const u8 raw[32]);
  bool split(const std::string& path, Entry& parent, std::string& name, std::string* err) const;
  bool put(const std::string& path, const u8* data, u32 len, bool with_data, Entry* out, std::string* err, const Stamp* stamp);
  static bool to_83(const std::string& name, u8 out[11]);

  ReadFn read_;
  WriteFn write_;
  u32 bps_ = 0, spc_ = 0, clusters_ = 0;
  int fat_bits_ = 0;
  u32 nfats_ = 0, fat_sectors_ = 0, root_entries_ = 0, root_cluster_ = 0;
  u64 fat_off_ = 0, root_off_ = 0, data_off_ = 0;
  std::vector<u8> fat_;             // the first FAT, cached; written to every copy
  std::vector<bool> fat_dirty_;     // per FAT sector
  u32 free_count_ = 0;              // clusters whose FAT entry is 0
  u32 next_free_ = 2;               // where the next allocation starts looking
  bool preserve_case_ = false;
  bool fresh_ = false;
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
