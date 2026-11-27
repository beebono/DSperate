// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_file.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "core/cart/miniz/miniz_tinfl.h"

namespace ds::sdl {

const char* save_ext(SaveKind k) {
  switch (k) {
    case SaveKind::Dsv: return ".dsv";
    case SaveKind::Srm: return ".srm";
    default: return ".sav";
  }
}
const char* save_kind_name(SaveKind k) { return save_ext(k) + 1; }

bool parse_save_kind(const std::string& v, SaveKind& out) {
  std::string s = !v.empty() && v[0] == '.' ? v.substr(1) : v;
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (SaveKind k : {SaveKind::Sav, SaveKind::Dsv, SaveKind::Srm})
    if (s == save_kind_name(k)) { out = k; return true; }
  return false;
}

namespace {
bool blank_image(const std::vector<uint8_t>& d) {
  if (d.empty() || (d[0] != 0x00 && d[0] != 0xFF)) return false;
  return std::all_of(d.begin(), d.end(), [&](uint8_t b) { return b == d[0]; });
}
} // namespace

std::vector<SaveFile> find_saves(const std::vector<std::string>& stems, const std::vector<SaveKind>& kinds) {
  std::vector<SaveFile> out;
  for (SaveKind k : kinds) {
    for (size_t i = 0; i < stems.size(); ++i) {
      if (stems[i].empty() || std::find(stems.begin(), stems.begin() + i, stems[i]) != stems.begin() + i) continue;
      const std::string path = stems[i] + save_ext(k);
      struct stat st;
      if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      SaveFile f;
      f.path = path;
      f.kind = k;
      f.bytes = static_cast<uint64_t>(st.st_size);
      f.mtime = static_cast<int64_t>(st.st_mtime);
      f.beside_rom = i > 0;
      std::vector<uint8_t> data;
      std::string err;
      if (st.st_size > 0 && read_save_file(path, data, err) == ReadResult::Ok) {
        f.blank = blank_image(data);
        f.compressed = is_rzip_file(path);
      } else {
        f.blank = st.st_size == 0;
      }
      out.push_back(std::move(f));
    }
  }
  return out;
}

int newest_save(const std::vector<SaveFile>& saves) {
  int best = -1;
  for (int i = 0; i < static_cast<int>(saves.size()); ++i) {
    const SaveFile& s = saves[static_cast<size_t>(i)];
    if (best < 0) { best = i; continue; }
    const SaveFile& b = saves[static_cast<size_t>(best)];
    if (s.blank != b.blank ? !s.blank : s.mtime > b.mtime) best = i;
  }
  return best;
}

bool is_rzip(const uint8_t* p, size_t n) {
  return n >= 20 && std::memcmp(p, "#RZIPv", 6) == 0 && p[7] == '#';
}

bool is_rzip_file(const std::string& path) {
  uint8_t h[20];
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const size_t n = std::fread(h, 1, sizeof h, f);
  std::fclose(f);
  return is_rzip(h, n);
}

// libretro-common's rzip_stream: "#RZIPv" + version + "#", the uncompressed
// chunk size (u32 LE), the total size (u64 LE); then chunks, each a u32 LE
// length and a zlib stream holding at most one chunk.
bool rzip_decode(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
  out.clear();
  if (!is_rzip(p, n) || p[6] != 1) return false;   // 2 is zstd
  const auto le = [&](size_t at, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = v << 8 | p[at + static_cast<size_t>(i)];
    return v;
  };
  const uint64_t chunk = le(8, 4), total = le(12, 8);
  if (chunk == 0 || chunk > (64u << 20) || total == 0 || total > (256u << 20)) return false;
  out.reserve(static_cast<size_t>(total));
  std::vector<uint8_t> buf(static_cast<size_t>(chunk));
  for (size_t at = 20; out.size() < total;) {
    if (n - at < 4) return false;
    const uint64_t csize = le(at, 4);
    at += 4;
    if (csize == 0 || csize > n - at) return false;
    const size_t got = tinfl_decompress_mem_to_mem(buf.data(), buf.size(), p + at, static_cast<size_t>(csize), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got == 0 || out.size() + got > total) return false;
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    at += static_cast<size_t>(csize);
  }
  return true;
}

ReadResult read_save_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
  out.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (errno == ENOENT) return ReadResult::Missing;
    err = "cannot open " + path + ": " + std::strerror(errno);
    return ReadResult::Error;
  }
  uint8_t buf[65536];
  for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + got);
  // A read cut short is not a save of another size.
  const bool read_error = std::ferror(f) != 0;
  std::fclose(f);
  if (read_error) { err = "cannot read " + path + " (stopped after " + std::to_string(out.size()) + " bytes)"; return ReadResult::Error; }
  if (is_rzip(out.data(), out.size())) {
    std::vector<uint8_t> plain;
    if (!rzip_decode(out.data(), out.size(), plain)) { err = path + " is a RetroArch compressed save that cannot be unpacked"; return ReadResult::Error; }
    out.swap(plain);
  }
  return ReadResult::Ok;
}

} // namespace ds::sdl
