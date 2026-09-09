// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Prints the RetroAchievements hash of each ROM named on the command line:
//
//   dsperate_cheevos_hash ~/ROMs/nds/*.nds
//
// The point is comparison against RetroAchievements' own database -- the hash
// is only correct if their server recognises it, and nothing short of looking
// it up proves that. Handles .zip the same way the emulator does, so the
// zipped and unzipped copies of one game can be checked against each other.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cheevos/cheevos_hash.h"
#include "core/cart/rom_source.h"
#include "core/cart/zip.h"

namespace {

bool ends_with(const std::string& s, const char* suffix) {
  const std::string t(suffix);
  if (s.size() < t.size()) return false;
  for (size_t i = 0; i < t.size(); ++i) {
    char a = s[s.size() - t.size() + i], b = t[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::unique_ptr<ds::cart::RomSource> open_rom(const std::string& path, std::string& err) {
  if (!ends_with(path, ".zip")) return ds::cart::RomSource::map_file(path, err);

  // zip.h works on a buffer, so the archive is slurped. That is the one thing
  // here the emulator does differently -- it maps a stored entry in place and
  // caches an inflated one to disk (zip_cache.h) -- but the bytes handed on
  // are the same bytes either way, which is the whole point of RomSource, so
  // the hash is too.
  std::vector<ds::u8> zip;
  {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open"; return nullptr; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); err = "empty"; return nullptr; }
    zip.resize(static_cast<size_t>(n));
    const bool ok = std::fread(zip.data(), 1, zip.size(), f) == zip.size();
    std::fclose(f);
    if (!ok) { err = "short read"; return nullptr; }
  }

  std::vector<ds::u8> bytes;
  std::string chosen;
  if (!ds::cart::extract_nds(zip.data(), zip.size(), bytes, err, &chosen)) return nullptr;
  if (!chosen.empty()) std::fprintf(stderr, "%s: using %s\n", path.c_str(), chosen.c_str());
  return ds::cart::RomSource::from_memory(std::move(bytes));
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <rom.nds|rom.zip> ...\n", argv[0]);
    return 2;
  }

  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string path = argv[i];
    std::string err;
    const auto src = open_rom(path, err);
    if (!src) {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
      ++bad;
      continue;
    }
    std::string hash;
    if (!ds::cheevos::rom_hash(*src, path, hash, err)) {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
      ++bad;
      continue;
    }
    std::printf("%s  %s\n", hash.c_str(), path.c_str());
  }
  return bad ? 1 : 0;
}
