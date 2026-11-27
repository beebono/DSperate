// SPDX-License-Identifier: GPL-3.0-or-later
// The battery save's file (frontend/sdl/save_file): which of .sav, .dsv and
// .srm are there, which is newest, and RetroArch's rzip read back.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "check.h"
#include "core/cart/miniz/miniz_tdef.h"
#include "frontend/sdl/save_file.h"

using namespace ds::sdl;

namespace {
void put(const std::string& path, const std::vector<uint8_t>& d, time_t mtime) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f);
  CHECK(std::fwrite(d.data(), 1, d.size(), f) == d.size());
  std::fclose(f);
  timeval tv[2] = {{mtime, 0}, {mtime, 0}};
  CHECK(utimes(path.c_str(), tv) == 0);
}
void le(std::vector<uint8_t>& o, uint64_t v, int n) { for (int i = 0; i < n; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
// RetroArch's layout, chunked as it chunks.
std::vector<uint8_t> rzip(const std::vector<uint8_t>& plain, uint32_t chunk) {
  std::vector<uint8_t> o = {'#', 'R', 'Z', 'I', 'P', 'v', 1, '#'};
  le(o, chunk, 4);
  le(o, plain.size(), 8);
  for (size_t at = 0; at < plain.size(); at += chunk) {
    size_t n = 0;
    const size_t take = std::min<size_t>(chunk, plain.size() - at);
    void* z = tdefl_compress_mem_to_heap(plain.data() + at, take, &n, TDEFL_WRITE_ZLIB_HEADER | 128);
    CHECK(z && n);
    le(o, n, 4);
    o.insert(o.end(), static_cast<uint8_t*>(z), static_cast<uint8_t*>(z) + n);
    std::free(z);   // plain malloc, as main.cpp frees the PNG
  }
  return o;
}
} // namespace

int main() {
  char tmpl[] = "/tmp/dsperate_save_file_XXXXXX";
  CHECK(mkdtemp(tmpl));
  const std::string dir = tmpl, saves = dir + "/saves", roms = dir + "/roms";
  CHECK(mkdir(saves.c_str(), 0755) == 0 && mkdir(roms.c_str(), 0755) == 0);

  std::vector<uint8_t> game(512 * 1024);
  for (size_t i = 0; i < game.size(); ++i) game[i] = static_cast<uint8_t>(i * 7 + (i >> 9));

  // rzip: several chunks and a short last one, back byte for byte.
  {
    const std::vector<uint8_t> z = rzip(game, 128 * 1024 + 100);
    CHECK(is_rzip(z.data(), z.size()));
    std::vector<uint8_t> back;
    CHECK(rzip_decode(z.data(), z.size(), back) && back == game);
    std::vector<uint8_t> cut(z.begin(), z.end() - 9);
    CHECK(!rzip_decode(cut.data(), cut.size(), back));
    std::vector<uint8_t> zstd = z;
    zstd[6] = 2;
    CHECK(!rzip_decode(zstd.data(), zstd.size(), back));
    CHECK(!is_rzip(game.data(), game.size()));
  }

  std::vector<uint8_t> out;
  std::string err;
  CHECK(read_save_file(saves + "/none.sav", out, err) == ReadResult::Missing);

  // Nothing yet, then one file: the one.
  const std::vector<std::string> stems = {saves + "/Game", roms + "/Game"};
  const std::vector<SaveKind> all = {SaveKind::Sav, SaveKind::Dsv, SaveKind::Srm};
  CHECK(find_saves(stems, all).empty());
  CHECK(newest_save({}) == -1);
  put(saves + "/Game.sav", game, 1000);
  {
    const auto f = find_saves(stems, all);
    CHECK(f.size() == 1 && f[0].kind == SaveKind::Sav && !f[0].beside_rom && !f[0].blank && f[0].bytes == game.size());
  }

  // A newer compressed .srm beside the ROM, and a still newer blank .dsv: the .srm is the newest with something in it.
  put(roms + "/Game.srm", rzip(game, 128 * 1024), 3000);
  put(saves + "/Game.dsv", std::vector<uint8_t>(game.size(), 0xFF), 4000);
  {
    const auto f = find_saves(stems, all);
    CHECK(f.size() == 3);
    CHECK(f[0].kind == SaveKind::Sav && f[1].kind == SaveKind::Dsv && f[2].kind == SaveKind::Srm);
    CHECK(f[1].blank && f[2].compressed && f[2].beside_rom && !f[2].blank);
    CHECK(newest_save(f) == 2);
    CHECK(read_save_file(f[2].path, out, err) == ReadResult::Ok && out == game);
    // The kinds asked for only: a GBA game takes .sav and .srm.
    CHECK(find_saves(stems, {SaveKind::Sav, SaveKind::Srm}).size() == 2);
  }
  // The same folder twice is one place.
  CHECK(find_saves({saves + "/Game", saves + "/Game"}, all).size() == 2);
  // All blank: the newest of them.
  CHECK(newest_save({SaveFile{"a", SaveKind::Sav, 1, 10, false, true, false}, SaveFile{"b", SaveKind::Srm, 1, 20, false, true, false}}) == 1);

  // A broken rzip is an error, never an empty save.
  {
    std::vector<uint8_t> bad = rzip(game, 128 * 1024);
    bad.resize(40);
    put(saves + "/Bad.srm", bad, 1);
    CHECK(read_save_file(saves + "/Bad.srm", out, err) == ReadResult::Error && !err.empty());
  }

  SaveKind k;
  CHECK(parse_save_kind("srm", k) && k == SaveKind::Srm);
  CHECK(parse_save_kind(".DSV", k) && k == SaveKind::Dsv);
  CHECK(!parse_save_kind("auto", k) && !parse_save_kind("", k));
  CHECK(std::strcmp(save_ext(SaveKind::Sav), ".sav") == 0 && std::strcmp(save_kind_name(SaveKind::Srm), "srm") == 0);

  for (const char* f : {"/saves/Game.sav", "/saves/Game.dsv", "/roms/Game.srm", "/saves/Bad.srm"}) std::remove((dir + f).c_str());
  rmdir(saves.c_str()); rmdir(roms.c_str()); rmdir(dir.c_str());
  std::puts("save_file: ok");
  return 0;
}
