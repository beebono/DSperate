// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_aes.h"
#include "core/io/dsi_sd.h"
#include "core/crypto/sha1.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ds::io {
namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }
void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
void wr32(u8* p, u32 v) { wr16(p, static_cast<u16>(v)); wr16(p + 2, static_cast<u16>(v >> 16)); }

// FAT date for 2000-01-01, the date the DSi's own files carry; no time.
constexpr u16 kFatDate = ((2000 - 1980) << 9) | (1 << 5) | 1;

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> parts_of(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) out.push_back(path.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

}  // namespace

// ---- FatVolume ---------------------------------------------------------------

bool FatVolume::open(ReadFn read, WriteFn write, std::string* err) {
  auto fail = [&](const char* m) { if (err) *err = m; bps_ = 0; return false; };
  read_ = std::move(read);
  write_ = std::move(write);
  u8 b[512];
  read_(0, 512, b);
  if (b[0x1FE] != 0x55 || b[0x1FF] != 0xAA) return fail("no boot sector signature");
  const u32 bps = rd16(b + 0x0B), spc = b[0x0D], rsv = rd16(b + 0x0E);
  const u32 nfats = b[0x10], root = rd16(b + 0x11), spf = rd16(b + 0x16);
  const u32 total = rd16(b + 0x13) ? rd16(b + 0x13) : rd32(b + 0x20);
  if (bps != 512 || spc == 0 || (spc & (spc - 1)) || nfats == 0 || spf == 0 || root == 0) return fail("not a FAT12/FAT16 volume");
  const u32 root_sectors = (root * 32 + bps - 1) / bps;
  const u32 data_sector = rsv + nfats * spf + root_sectors;
  if (total <= data_sector) return fail("volume smaller than its own metadata");
  const u32 clusters = (total - data_sector) / spc;
  if (clusters >= 65525) return fail("FAT32 is not used on the DSi");
  bps_ = bps; spc_ = spc; nfats_ = nfats; fat_sectors_ = spf; root_entries_ = root;
  clusters_ = clusters;
  fat_bits_ = clusters < 4085 ? 12 : 16;
  fat_off_ = static_cast<u64>(rsv) * bps;
  root_off_ = fat_off_ + static_cast<u64>(nfats) * spf * bps;
  data_off_ = static_cast<u64>(data_sector) * bps;
  fat_.assign(static_cast<size_t>(spf) * bps, 0);
  for (u32 s = 0; s < spf; ++s) read_(fat_off_ + static_cast<u64>(s) * bps, bps, fat_.data() + s * bps);
  fat_dirty_.assign(spf, false);
  return true;
}

u32 FatVolume::fat_get(u32 c) const {
  if (fat_bits_ == 12) {
    const size_t o = c + c / 2;
    if (o + 1 >= fat_.size()) return 0xFFF;
    const u32 v = fat_[o] | (fat_[o + 1] << 8);
    return (c & 1) ? v >> 4 : v & 0xFFF;
  }
  const size_t o = static_cast<size_t>(c) * 2;
  return o + 1 < fat_.size() ? rd16(&fat_[o]) : 0xFFFF;
}

void FatVolume::fat_set(u32 c, u32 v) {
  size_t o;
  if (fat_bits_ == 12) {
    o = c + c / 2;
    if (c & 1) { fat_[o] = static_cast<u8>((fat_[o] & 0x0F) | ((v << 4) & 0xF0)); fat_[o + 1] = static_cast<u8>(v >> 4); }
    else       { fat_[o] = static_cast<u8>(v); fat_[o + 1] = static_cast<u8>((fat_[o + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
  } else {
    o = static_cast<size_t>(c) * 2;
    wr16(&fat_[o], static_cast<u16>(v));
  }
  fat_dirty_[o / bps_] = true;
  fat_dirty_[(o + 1) / bps_] = true;
}

void FatVolume::fat_flush() {
  for (u32 s = 0; s < fat_sectors_; ++s) {
    if (!fat_dirty_[s]) continue;
    for (u32 k = 0; k < nfats_; ++k)
      write_(fat_off_ + (static_cast<u64>(k) * fat_sectors_ + s) * bps_, bps_, fat_.data() + s * bps_);
    fat_dirty_[s] = false;
  }
}

u32 FatVolume::free_clusters() const {
  u32 n = 0;
  for (u32 c = 2; c < clusters_ + 2; ++c) n += fat_get(c) == 0;
  return n;
}

std::vector<u32> FatVolume::chain(u32 first) const {
  std::vector<u32> out;
  const u32 eoc = fat_bits_ == 12 ? 0xFF8 : 0xFFF8;
  for (u32 c = first; c >= 2 && c < clusters_ + 2 && out.size() <= clusters_; c = fat_get(c)) {
    out.push_back(c);
    if (fat_get(c) >= eoc) break;
  }
  return out;
}

bool FatVolume::alloc_chain(u32 count, u32& first) {
  first = 0;
  if (count == 0) return true;
  std::vector<u32> got;
  for (u32 c = 2; c < clusters_ + 2 && got.size() < count; ++c)
    if (fat_get(c) == 0) got.push_back(c);
  if (got.size() < count) return false;
  const u32 eoc = fat_bits_ == 12 ? 0xFFF : 0xFFFF;
  for (size_t i = 0; i < got.size(); ++i) fat_set(got[i], i + 1 < got.size() ? got[i + 1] : eoc);
  first = got[0];
  return true;
}

void FatVolume::free_chain(u32 first) {
  for (u32 c : chain(first)) fat_set(c, 0);
}

std::vector<u8> FatVolume::dir_bytes(const Entry& dir) const {
  std::vector<u8> out;
  if (dir.cluster == 0) {
    out.resize((root_entries_ * 32 + bps_ - 1) / bps_ * bps_);
    for (size_t s = 0; s < out.size(); s += bps_) read_(root_off_ + s, bps_, out.data() + s);
    return out;
  }
  const u32 cs = cluster_bytes();
  for (u32 c : chain(dir.cluster)) {
    const size_t at = out.size();
    out.resize(at + cs);
    for (u32 s = 0; s < cs; s += bps_) read_(cluster_offset(c) + s, bps_, out.data() + at + s);
  }
  return out;
}

std::vector<FatVolume::Entry> FatVolume::list(const Entry& dir) const {
  std::vector<Entry> out;
  const std::vector<u8> d = dir_bytes(dir);
  const std::vector<u32> cl = dir.cluster ? chain(dir.cluster) : std::vector<u32>{};
  const u32 cs = cluster_bytes();
  for (size_t i = 0; i + 32 <= d.size(); i += 32) {
    const u8* e = &d[i];
    if (e[0] == 0x00) break;
    if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
    Entry en;
    std::string base(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e + 8), 3);
    if (static_cast<u8>(base[0]) == 0x05) base[0] = static_cast<char>(0xE5);
    base.erase(base.find_last_not_of(' ') + 1);
    ext.erase(ext.find_last_not_of(' ') + 1);
    en.name = ext.empty() ? base : base + "." + ext;
    en.attr = e[11];
    en.cluster = rd16(e + 26);
    en.size = rd32(e + 28);
    en.dirent = dir.cluster == 0 ? root_off_ + i : cluster_offset(cl[i / cs]) + i % cs;
    out.push_back(std::move(en));
  }
  return out;
}

bool FatVolume::lookup(const std::string& path, Entry& out) const {
  Entry cur;
  cur.attr = 0x10;
  for (const std::string& p : parts_of(path)) {
    if (!cur.dir()) return false;
    const std::string want = upper(p);
    bool found = false;
    for (const Entry& e : list(cur)) {
      if (e.name != want) continue;
      cur = e;
      found = true;
      break;
    }
    if (!found) return false;
  }
  out = cur;
  return true;
}

bool FatVolume::read(const Entry& file, std::vector<u8>& out) const {
  out.clear();
  if (file.dir()) return false;
  const u32 cs = cluster_bytes();
  const std::vector<u32> cl = chain(file.cluster);
  if (static_cast<u64>(cl.size()) * cs < file.size) return false;   // a chain shorter than the size: a broken file
  out.resize(static_cast<size_t>(cl.size()) * cs);
  for (size_t k = 0; k < cl.size(); ++k)
    for (u32 s = 0; s < cs; s += bps_) read_(cluster_offset(cl[k]) + s, bps_, out.data() + k * cs + s);
  out.resize(file.size);
  return true;
}

void FatVolume::walk(const std::function<void(const std::string&, const Entry&)>& fn) const {
  std::vector<std::pair<std::string, Entry>> stack;
  Entry root;
  root.attr = 0x10;
  stack.emplace_back("", root);
  while (!stack.empty()) {
    auto [base, dir] = stack.back();
    stack.pop_back();
    for (const Entry& e : list(dir)) {
      if (e.name == "." || e.name == "..") continue;
      const std::string path = base + "/" + e.name;
      fn(path, e);
      if (e.dir() && e.cluster) stack.emplace_back(path, e);
    }
  }
}

std::vector<u64> FatVolume::extents(const Entry& file) const {
  std::vector<u64> out;
  for (u32 c : chain(file.cluster)) out.push_back(cluster_offset(c));
  return out;
}

bool FatVolume::to_83(const std::string& name, u8 out[11]) {
  std::memset(out, ' ', 11);
  const size_t dot = name.find_last_of('.');
  const std::string base = upper(dot == std::string::npos ? name : name.substr(0, dot));
  const std::string ext = upper(dot == std::string::npos ? "" : name.substr(dot + 1));
  if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
  for (char c : base + ext)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '~')) return false;
  std::memcpy(out, base.data(), base.size());
  std::memcpy(out + 8, ext.data(), ext.size());
  return true;
}

bool FatVolume::split(const std::string& path, Entry& parent, std::string& name, std::string* err) const {
  std::vector<std::string> p = parts_of(path);
  if (p.empty()) { if (err) *err = "empty path"; return false; }
  name = p.back();
  p.pop_back();
  std::string dir;
  for (const std::string& s : p) dir += "/" + s;
  if (!lookup(dir, parent) || !parent.dir()) { if (err) *err = "no directory " + (dir.empty() ? "/" : dir); return false; }
  return true;
}

void FatVolume::write_entry(u64 dirent, const u8 raw[32]) {
  const u64 sec = dirent / bps_ * bps_;
  u8 b[512];
  read_(sec, bps_, b);
  std::memcpy(b + (dirent - sec), raw, 32);
  write_(sec, bps_, b);
}

bool FatVolume::add_entry(const Entry& parent, const u8 raw[32], u64& dirent_out) {
  const std::vector<u8> d = dir_bytes(parent);
  const std::vector<u32> cl = parent.cluster ? chain(parent.cluster) : std::vector<u32>{};
  const u32 cs = cluster_bytes();
  const size_t limit = parent.cluster ? d.size() : static_cast<size_t>(root_entries_) * 32;
  for (size_t i = 0; i + 32 <= limit; i += 32) {
    if (d[i] != 0x00 && d[i] != 0xE5) continue;
    dirent_out = parent.cluster ? cluster_offset(cl[i / cs]) + i % cs : root_off_ + i;
    write_entry(dirent_out, raw);
    return true;
  }
  if (!parent.cluster || cl.empty()) return false;   // the root directory is full
  // Grow the directory by a zeroed cluster and put the entry at its start.
  u32 c;
  if (!alloc_chain(1, c)) return false;
  fat_set(cl.back(), c);
  std::vector<u8> zero(bps_, 0);
  for (u32 s = 0; s < cs; s += bps_) write_(cluster_offset(c) + s, bps_, zero.data());
  dirent_out = cluster_offset(c);
  write_entry(dirent_out, raw);
  return true;
}

bool FatVolume::write(const std::string& path, const u8* data, u32 len, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = path + ": " + m; return false; };
  Entry parent;
  std::string name;
  if (!split(path, parent, name, err)) return false;
  u8 raw[32] = {};
  if (!to_83(name, raw)) return fail("not an 8.3 name");

  Entry existing;
  const bool exists = lookup(path, existing);
  if (exists && existing.dir()) return fail("is a directory");
  const u32 cs = cluster_bytes();
  const u32 need = (len + cs - 1) / cs;
  const u32 have = exists ? static_cast<u32>(chain(existing.cluster).size()) : 0;
  if (need > have && free_clusters() < need - have) return fail("not enough free space");

  u32 first = exists ? existing.cluster : 0;
  if (need != have) {
    if (exists && existing.cluster) free_chain(existing.cluster);
    if (!alloc_chain(need, first)) return fail("allocation failed");
  }
  std::vector<u8> buf(bps_);
  const std::vector<u32> cl = chain(first);
  for (u32 k = 0; k < need; ++k)
    for (u32 s = 0; s < cs; s += bps_) {
      const u64 at = static_cast<u64>(k) * cs + s;
      std::memset(buf.data(), 0, bps_);
      if (at < len) std::memcpy(buf.data(), data + at, static_cast<size_t>(std::min<u64>(bps_, len - at)));
      write_(cluster_offset(cl[k]) + s, bps_, buf.data());
    }
  fat_flush();

  if (exists) {
    u8 b[512];
    const u64 sec = existing.dirent / bps_ * bps_;
    read_(sec, bps_, b);
    u8* e = b + (existing.dirent - sec);
    wr16(e + 26, static_cast<u16>(first));
    wr32(e + 28, len);
    write_(sec, bps_, b);
  } else {
    raw[11] = 0x20;   // archive
    wr16(raw + 16, kFatDate);   // created
    wr16(raw + 18, kFatDate);   // accessed
    wr16(raw + 24, kFatDate);   // modified
    wr16(raw + 26, static_cast<u16>(first));
    wr32(raw + 28, len);
    u64 dirent;
    if (!add_entry(parent, raw, dirent)) { free_chain(first); fat_flush(); return fail("directory is full"); }
    fat_flush();
  }
  return true;
}

bool FatVolume::mkdir(const std::string& path, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = path + ": " + m; return false; };
  Entry e;
  if (lookup(path, e)) return e.dir() ? true : fail("exists as a file");
  Entry parent;
  std::string name;
  if (!split(path, parent, name, err)) return false;
  u8 raw[32] = {};
  if (!to_83(name, raw)) return fail("not an 8.3 name");
  u32 c;
  if (!alloc_chain(1, c)) return fail("not enough free space");

  // The new directory's cluster: "." and "..", the rest zero.
  std::vector<u8> sec(bps_, 0);
  auto dot = [&](u8* p, const char* n, u32 cluster) {
    std::memset(p, ' ', 11);
    std::memcpy(p, n, std::strlen(n));
    p[11] = 0x10;
    wr16(p + 16, kFatDate); wr16(p + 18, kFatDate); wr16(p + 24, kFatDate);
    wr16(p + 26, static_cast<u16>(cluster));
  };
  for (u32 s = 0; s < cluster_bytes(); s += bps_) {
    std::memset(sec.data(), 0, bps_);
    if (s == 0) { dot(sec.data(), ".", c); dot(sec.data() + 32, "..", parent.cluster); }
    write_(cluster_offset(c) + s, bps_, sec.data());
  }
  raw[11] = 0x10;
  wr16(raw + 16, kFatDate); wr16(raw + 18, kFatDate); wr16(raw + 24, kFatDate);
  wr16(raw + 26, static_cast<u16>(c));
  u64 dirent;
  if (!add_entry(parent, raw, dirent)) { free_chain(c); fat_flush(); return fail("directory is full"); }
  fat_flush();
  return true;
}

bool FatVolume::remove(const std::string& path, std::string* err) {
  Entry e;
  if (!lookup(path, e)) return true;
  if (e.dirent == 0) { if (err) *err = "cannot remove the root"; return false; }
  if (e.dir() && e.cluster) {
    for (const Entry& c : list(e)) {
      if (c.name == "." || c.name == "..") continue;
      if (!remove(path + "/" + c.name, err)) return false;
    }
  }
  if (e.cluster) free_chain(e.cluster);
  fat_flush();
  u8 raw[32];
  const u64 sec = e.dirent / bps_ * bps_;
  u8 b[512];
  read_(sec, bps_, b);
  std::memcpy(raw, b + (e.dirent - sec), 32);
  raw[0] = 0xE5;
  write_entry(e.dirent, raw);
  return true;
}

// ---- NandFs ------------------------------------------------------------------

namespace {
void bswap16(u8* dst, const u8* src) { for (int i = 0; i < 16; ++i) dst[i] = src[15 - i]; }
}  // namespace

bool NandFs::mount(NandImage& nand, const u8* bios7i, std::string* err) {
  nand_ = &nand;
  const u64 id = nand.console_id();
  const u32 lo = static_cast<u32>(id), hi = static_cast<u32>(id >> 32);
  u8 kx[16], ky[16], tmp[16];

  // The filesystem key and counter (melonDS DSi_NAND.cpp NANDImage::NANDImage).
  wr32(kx, lo); wr32(kx + 4, lo ^ 0x24EE6906); wr32(kx + 8, hi ^ 0xE65B601D); wr32(kx + 12, hi);
  wr32(ky, 0x0AB9DC76); wr32(ky + 4, 0xBD4DC4D3); wr32(ky + 8, 0x202DDD1D); wr32(ky + 12, 0xE1A00005);
  DsiAes::derive_normal_key(kx, ky, tmp);
  u8 key[16];
  bswap16(key, tmp);
  AES_init_ctx(&fat_ctx_, key);
  u8 digest[20];
  crypto::sha1(nand.emmc_cid(), 16, digest);
  bswap16(fat_iv_, digest);

  // The ES key, from the DSi ARM7 BIOS.
  es_key_ok_ = bios7i != nullptr;
  if (es_key_ok_) {
    wr32(kx, 0x4E00004A); wr32(kx + 4, 0x4A00004E); wr32(kx + 8, hi ^ 0xC80C4B72); wr32(kx + 12, lo);
    DsiAes::derive_normal_key(kx, bios7i + 0x8308, tmp);
    bswap16(es_key_, tmp);
  }

  u8 mbr[512];
  crypt_read(0, 512, mbr);
  if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) { if (err) *err = "the MBR did not decrypt (wrong console ID or CID in the footer)"; return false; }
  auto part = [&](int i, FatVolume& vol, bool required) {
    const u8* p = mbr + 0x1BE + i * 16;
    const u64 base = static_cast<u64>(rd32(p + 8)) * 512;
    (i == 0 ? main_base_ : photo_base_) = base;
    if (!p[4] || !rd32(p + 12)) return !required;
    std::string why;
    const bool ok = vol.open([this, base](u64 o, u32 n, u8* out) { crypt_read(base + o, n, out); },
                             [this, base](u64 o, u32 n, const u8* in) { crypt_write(base + o, n, in); }, &why);
    if (!ok && required && err) *err = "partition " + std::to_string(i) + ": " + why;
    return ok || !required;
  };
  if (!part(0, main_, true)) return false;
  part(1, photo_, false);
  return true;
}

// AES-CTR over byte-reversed 16-byte blocks, the counter the base IV plus the
// block's position in the image (so any sector decrypts on its own).
void NandFs::xcrypt(u64 offset, u8* buf, u32 len) const {
  for (u32 i = 0; i < len; i += 16) {
    u8 ctr[16];
    std::memcpy(ctr, fat_iv_, 16);
    u64 add = (offset + i) >> 4;
    unsigned carry = 0;
    for (int k = 15; k >= 0; --k) {
      const unsigned v = ctr[k] + static_cast<unsigned>(add & 0xFF) + carry;
      ctr[k] = static_cast<u8>(v);
      carry = v >> 8;
      add >>= 8;
    }
    AES_ECB_encrypt(&fat_ctx_, ctr);
    for (int k = 0; k < 16; ++k) buf[i + k] ^= ctr[15 - k];
  }
}

void NandFs::crypt_read(u64 offset, u32 len, u8* out) {
  nand_->peek(offset, len, out);
  xcrypt(offset, out, len);
}

void NandFs::crypt_write(u64 offset, u32 len, const u8* in) {
  std::vector<u8> buf(in, in + len);
  xcrypt(offset, buf.data(), len);
  nand_->poke(offset, len, buf.data());
}

void NandFs::es_encrypt(u8* data, u32 len, const u8 nonce[12]) const {
  for (int i = 0; i < 12; ++i) data[len + 0x1C - i] = nonce[i];
  AES_ctx ctx;
  u8 iv[16], mac[16];
  iv[0] = 0x02;
  for (int i = 0; i < 12; ++i) iv[1 + i] = data[len + 0x1C - i];
  iv[13] = 0x00; iv[14] = 0x00; iv[15] = 0x01;
  AES_init_ctx_iv(&ctx, es_key_, iv);

  const u32 blklen = (len + 0xF) & ~0xFu;
  mac[0] = 0x3A;
  for (int i = 1; i < 13; ++i) mac[i] = iv[i];
  mac[13] = static_cast<u8>(blklen >> 16); mac[14] = static_cast<u8>(blklen >> 8); mac[15] = static_cast<u8>(blklen);
  AES_ECB_encrypt(&ctx, mac);

  const u32 coarse = len & ~0xFu;
  for (u32 i = 0; i < coarse; i += 16) {
    u8 tmp[16];
    bswap16(tmp, &data[i]);
    for (int k = 0; k < 16; ++k) mac[k] ^= tmp[k];
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    AES_ECB_encrypt(&ctx, mac);
    bswap16(&data[i], tmp);
  }
  if (const u32 rem = len - coarse) {
    u8 r[16] = {};
    for (u32 i = 0; i < rem; ++i) r[15 - i] = data[coarse + i];
    for (int k = 0; k < 16; ++k) mac[k] ^= r[k];
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    AES_ECB_encrypt(&ctx, mac);
    for (u32 i = 0; i < rem; ++i) data[coarse + i] = r[15 - i];
  }
  ctx.Iv[13] = 0x00; ctx.Iv[14] = 0x00; ctx.Iv[15] = 0x00;
  AES_CTR_xcrypt_buffer(&ctx, mac, 16);
  bswap16(&data[len], mac);

  u8 footer[16] = {};
  iv[0] = 0x00; iv[1] = 0x00; iv[2] = 0x00;
  for (int i = 0; i < 12; ++i) iv[3 + i] = data[len + 0x1C - i];
  iv[15] = 0x00;
  footer[15] = 0x3A;
  footer[2] = static_cast<u8>(len >> 16); footer[1] = static_cast<u8>(len >> 8); footer[0] = static_cast<u8>(len);
  AES_ctx_set_iv(&ctx, iv);
  AES_CTR_xcrypt_buffer(&ctx, footer, 16);
  data[len + 0x10] = footer[15];
  data[len + 0x1D] = footer[2];
  data[len + 0x1E] = footer[1];
  data[len + 0x1F] = footer[0];
}

bool NandFs::es_decrypt(u8* data, u32 len) const {
  AES_ctx ctx;
  u8 iv[16], mac[16];
  iv[0] = 0x02;
  for (int i = 0; i < 12; ++i) iv[1 + i] = data[len + 0x1C - i];
  iv[13] = 0x00; iv[14] = 0x00; iv[15] = 0x01;
  AES_init_ctx_iv(&ctx, es_key_, iv);

  const u32 blklen = (len + 0xF) & ~0xFu;
  mac[0] = 0x3A;
  for (int i = 1; i < 13; ++i) mac[i] = iv[i];
  mac[13] = static_cast<u8>(blklen >> 16); mac[14] = static_cast<u8>(blklen >> 8); mac[15] = static_cast<u8>(blklen);
  AES_ECB_encrypt(&ctx, mac);

  const u32 coarse = len & ~0xFu;
  for (u32 i = 0; i < coarse; i += 16) {
    u8 tmp[16];
    bswap16(tmp, &data[i]);
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    for (int k = 0; k < 16; ++k) mac[k] ^= tmp[k];
    AES_ECB_encrypt(&ctx, mac);
    bswap16(&data[i], tmp);
  }
  if (const u32 rem = len - coarse) {
    const u32 ivnum = (coarse >> 4) + 1;
    iv[13] = static_cast<u8>(ivnum >> 16); iv[14] = static_cast<u8>(ivnum >> 8); iv[15] = static_cast<u8>(ivnum);
    u8 r[16] = {};
    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    for (u32 i = 0; i < rem; ++i) r[15 - i] = data[coarse + i];
    AES_ctx_set_iv(&ctx, iv);
    AES_CTR_xcrypt_buffer(&ctx, r, 16);
    for (int k = 0; k < 16; ++k) mac[k] ^= r[k];
    AES_ECB_encrypt(&ctx, mac);
    for (u32 i = 0; i < rem; ++i) data[coarse + i] = r[15 - i];
  }
  ctx.Iv[13] = 0x00; ctx.Iv[14] = 0x00; ctx.Iv[15] = 0x00;
  AES_CTR_xcrypt_buffer(&ctx, mac, 16);

  u8 footer[16];
  iv[0] = 0x00; iv[1] = 0x00; iv[2] = 0x00;
  for (int i = 0; i < 12; ++i) iv[3 + i] = data[len + 0x1C - i];
  iv[15] = 0x00;
  bswap16(footer, &data[len + 0x10]);
  AES_ctx_set_iv(&ctx, iv);
  AES_CTR_xcrypt_buffer(&ctx, footer, 16);
  data[len + 0x10] = footer[15];
  data[len + 0x1D] = footer[2];
  data[len + 0x1E] = footer[1];
  data[len + 0x1F] = footer[0];
  if ((footer[0] | (footer[1] << 8) | (footer[2] << 16)) != static_cast<int>(len)) return false;
  for (int i = 0; i < 16; ++i)
    if (data[len + i] != mac[15 - i]) return false;
  return true;
}

}  // namespace ds::io
