// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_nand_launch.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_sd.h"
#include "core/io/dsi_title_install.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace ds::io {
namespace fs = std::filesystem;
namespace {

constexpr char kMagic[8] = {'D', 'S', 'P', 'R', 'S', 'T', 'U', 'B'};
constexpr u32 kVersion = 1;
constexpr size_t kStubBytes = 64;

std::string hex8(u32 v) { char b[9]; std::snprintf(b, sizeof b, "%08x", v); return b; }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }

void utf8_append(std::string& s, u32 cp) {
  if (cp < 0x80) s += static_cast<char>(cp);
  else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// The first line of one of the banner's UTF-16 titles.
std::string banner_line(const u8* t) {
  std::string s;
  for (int i = 0; i < 0x80; ++i) {
    const u32 c = t[i * 2] | (t[i * 2 + 1] << 8);
    if (c == 0 || c == '\n') break;
    if (c >= 0xD800 && c <= 0xDFFF) continue;   // no surrogates in a DSi banner
    utf8_append(s, c);
  }
  return s;
}

// A name a FAT card, ext4 and a frontend's scraper all accept: no path or
// wildcard characters, no control characters, single spaces, no trailing dot.
std::string file_safe(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c < 0x20 || std::strchr("/\\:*?\"<>|", c)) c = ' ';
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += static_cast<char>(c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out;
}

bool mount(NandFs& nfs, NandImage& nand, const u8* bios7i, std::string* err) {
  std::string why;
  if (!nfs.mount(nand, bios7i, &why)) { if (err) *err = "the NAND cannot be read: " + why; return false; }
  return true;
}

bool app_entry(NandFs& nfs, u32 title_lo, FatVolume::Entry& app, u32& content_id) {
  FatVolume::Entry dir;
  if (!nfs.main().lookup("/title/00030004/" + hex8(title_lo) + "/content", dir) || !dir.dir()) return false;
  for (const FatVolume::Entry& e : nfs.main().list(dir)) {
    if (e.dir() || e.name.size() != 12 || e.name.compare(8, 4, ".APP") != 0) continue;
    char* end = nullptr;
    const unsigned long v = std::strtoul(e.name.substr(0, 8).c_str(), &end, 16);
    if (end && *end == 0) { app = e; content_id = static_cast<u32>(v); return true; }
  }
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::vector<NandTitle> nand_dsiware_titles(NandImage& nand, const u8* bios7i) {
  std::vector<NandTitle> out;
  NandFs nfs;
  if (!mount(nfs, nand, bios7i, nullptr)) return out;
  for (const DsiWareUsage& u : dsiware_usage(nfs.main())) {
    NandTitle t;
    t.title_lo = static_cast<u32>(std::strtoul(u.id.c_str(), nullptr, 16));
    t.code = u.code;
    FatVolume::Entry app;
    if (!app_entry(nfs, t.title_lo, app, t.content_id)) continue;   // a title directory without its .app: nothing to start
    u8 hdr[0x70];
    if (app.size >= sizeof hdr && nfs.main().read_part(app, 0, sizeof hdr, hdr)) {
      const u32 at = rd32(hdr + 0x68);
      u8 titles[0x200];
      // Titles 0 (Japanese) and 1 (English) of the banner.
      if (at && static_cast<u64>(at) + 0x240 + sizeof titles <= app.size && nfs.main().read_part(app, at + 0x240, sizeof titles, titles)) {
        t.name = file_safe(banner_line(titles + 0x100));
        if (t.name.empty()) t.name = file_safe(banner_line(titles));
      }
    }
    if (t.name.empty()) t.name = t.code;
    out.push_back(std::move(t));
  }
  return out;
}

bool nand_read_title_app(NandImage& nand, const u8* bios7i, u32 title_lo, std::vector<u8>& srl, u32& content_id, std::string* err) {
  NandFs nfs;
  if (!mount(nfs, nand, bios7i, err)) return false;
  FatVolume::Entry app;
  if (!app_entry(nfs, title_lo, app, content_id)) {
    if (err) *err = "title " + hex8(title_lo) + " is not installed on the NAND";
    return false;
  }
  if (!nfs.main().read(app, srl)) { if (err) *err = "title " + hex8(title_lo) + ": its .app cannot be read"; return false; }
  return true;
}

bool nand_boot_blobs(NandImage& nand, const u8* bios7i, std::vector<u8>& out, std::string* err) {
  NandFs nfs;
  if (!mount(nfs, nand, bios7i, err)) return false;
  auto file = [&](const char* path, std::vector<u8>& data) {
    FatVolume::Entry e;
    return nfs.main().lookup(path, e) && nfs.main().read(e, data);
  };
  std::vector<u8> c0, c1, hn, hs;
  const bool have0 = file("/shared1/TWLCFG0.dat", c0) && c0.size() >= 0x1B0;
  const bool have1 = file("/shared1/TWLCFG1.dat", c1) && c1.size() >= 0x1B0;
  if (!have0 && !have1) { if (err) *err = "the NAND has no TWLCFG"; return false; }
  const std::vector<u8>& cfg = (have1 && (!have0 || c1[0x81] > c0[0x81])) ? c1 : c0;   // byte 0x81 counts the saves
  if (!file("/sys/HWINFO_N.dat", hn) || hn.size() < 0x9C || !file("/sys/HWINFO_S.dat", hs) || hs.size() < 0xA0) {
    if (err) *err = "the NAND has no HWINFO_N/HWINFO_S";
    return false;
  }
  out.clear();
  out.insert(out.end(), cfg.begin() + 0x88, cfg.begin() + 0x1B0);
  out.insert(out.end(), hn.begin() + 0x88, hn.begin() + 0x9C);
  out.insert(out.end(), hs.begin() + 0x88, hs.begin() + 0xA0);
  return true;
}

bool NandShortcut::from(const NandImage& nand) const {
  return nand.valid() && console_id == nand.console_id() && std::memcmp(cid, nand.emmc_cid(), sizeof cid) == 0;
}

bool is_shortcut_name(const std::string& path) {
  const size_t n = std::strlen(kShortcutSuffix);
  return path.size() > n && lower(path.substr(path.size() - n)) == kShortcutSuffix;
}

bool read_shortcut(const std::string& path, NandShortcut& out) {
  std::ifstream f(path, std::ios::binary);
  u8 b[kStubBytes];
  if (!f.read(reinterpret_cast<char*>(b), sizeof b) || std::memcmp(b, kMagic, sizeof kMagic) != 0 || rd32(b + 8) != kVersion) return false;
  out.title_lo = rd32(b + 12);
  out.title_hi = rd32(b + 16);
  std::memcpy(out.cid, b + 20, sizeof out.cid);
  std::memcpy(&out.console_id, b + 36, sizeof out.console_id);
  return true;
}

ShortcutSync sync_shortcuts(const std::string& dir, NandImage* nand, const u8* bios7i, bool enabled) {
  ShortcutSync r;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) { r.notes.push_back((dir.empty() ? std::string("no games folder") : dir + " is not a folder")); return r; }

  // What should be there: file name -> title.
  std::map<std::string, NandTitle> want;
  if (enabled) {
    if (!nand || !nand->valid()) { r.notes.push_back("no NAND to list"); return r; }
    const std::vector<NandTitle> titles = nand_dsiware_titles(*nand, bios7i);
    std::map<std::string, int> uses;
    for (const NandTitle& t : titles) uses[lower(t.name)]++;
    for (const NandTitle& t : titles) {
      const std::string stem = uses[lower(t.name)] > 1 ? t.name + " (" + t.code + ")" : t.name;
      want[stem + kShortcutSuffix] = t;
    }
  }

  // What is there: keep a shortcut that names a wanted title under its wanted
  // name, remove every other .dspr.nds.
  std::map<std::string, bool> present;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    if (!is_shortcut_name(name) || !e.is_regular_file(ec)) continue;
    NandShortcut s;
    const auto it = want.find(name);
    if (it != want.end() && read_shortcut(e.path().string(), s) && s.from(*nand) && s.title_lo == it->second.title_lo) {
      present[name] = true;
      continue;
    }
    if (fs::remove(e.path(), ec)) r.removed++;
    else r.notes.push_back(name + ": cannot be removed (" + ec.message() + ")");
    ec.clear();
  }
  for (const auto& [name, t] : want) {
    if (present.count(name)) continue;
    u8 b[kStubBytes] = {};
    std::memcpy(b, kMagic, sizeof kMagic);
    const u32 fields[3] = {kVersion, t.title_lo, 0x00030004};
    std::memcpy(b + 8, fields, sizeof fields);
    std::memcpy(b + 20, nand->emmc_cid(), 16);
    const u64 id = nand->console_id();
    std::memcpy(b + 36, &id, sizeof id);
    std::ofstream f(fs::path(dir) / name, std::ios::binary | std::ios::trunc);
    if (f.write(reinterpret_cast<const char*>(b), sizeof b)) r.written++;
    else r.notes.push_back(name + ": cannot be written");
  }
  return r;
}

}  // namespace ds::io
