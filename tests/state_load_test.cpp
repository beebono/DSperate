// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What loading a save state may and may not do to the player's data: a load
// that fails part way leaves the machine as it was, a load never marks the
// battery save as needing a write, and a file is replaced whole or not at all.
#include "core/nds.h"
#include "core/cart/cart.h"
#include "core/safe_file.h"
#include "core/state/state.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace ds;

namespace {

std::unique_ptr<NDS> machine() {
  auto p = std::make_unique<NDS>();
  std::vector<u8> rom(0x4000);
  CHECK(p->load_rom_image(std::move(rom)));
  p->reset();
  std::vector<u8> save(8192, 0x11);
  CHECK(p->cart->load_save(save.data(), save.size()).fitted);
  p->run_frame();
  return p;
}

std::vector<u8> snapshot(NDS& nds) {
  state::Writer w; std::string err;
  CHECK(nds.save_state(w, err));
  return w.data();
}

void test_failed_load_puts_the_machine_back() {
  auto p = machine();
  NDS& nds = *p;
  const std::vector<u8> early = snapshot(nds);
  for (int i = 0; i < 3; ++i) nds.run_frame();
  const std::vector<u8> now = snapshot(nds);
  CHECK(now != early);

  // Cut inside the machine's chunks, at several depths: each fails after the overwrite began.
  for (const size_t cut : {early.size() - 16, early.size() * 3 / 4, early.size() / 2, early.size() / 4}) {
    state::Reader r(early.data(), cut);
    std::string err;
    CHECK(!nds.load_state(r, err));
    CHECK(!err.empty());
    CHECK(snapshot(nds) == now);
    CHECK(!nds.cart->sram_dirty() && !nds.cart->sram_from_state());
  }
  // And it still runs, as a machine that never saw the bad file does.
  auto q = machine();
  for (int i = 0; i < 3; ++i) q->run_frame();
  CHECK(snapshot(*q) == now);
  nds.run_frame(); q->run_frame();
  CHECK(snapshot(nds) == snapshot(*q));
}

void test_load_is_not_a_save() {
  auto p = machine();
  NDS& nds = *p;
  const std::vector<u8> same = snapshot(nds);
  std::string err;
  { state::Reader r(same.data(), same.size()); CHECK(nds.load_state(r, err)); }
  CHECK(!nds.cart->sram_dirty() && !nds.cart->sram_from_state());   // the same save memory: nothing to say

  // A state from a session whose save differs.
  auto o = std::make_unique<NDS>();
  { std::vector<u8> rom(0x4000); CHECK(o->load_rom_image(std::move(rom))); }
  o->reset();
  std::vector<u8> other(8192, 0x22);
  CHECK(o->cart->load_save(other.data(), other.size()).fitted);
  o->run_frame();
  const std::vector<u8> theirs = snapshot(*o);
  { state::Reader r(theirs.data(), theirs.size()); CHECK(nds.load_state(r, err)); }
  CHECK(nds.cart->sram()[0] == 0x22);          // the game goes on from the state's memory
  CHECK(!nds.cart->sram_dirty());              // but the file is not due a write
  CHECK(nds.cart->sram_from_state());          // and the frontend knows the file is another save
}

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void test_atomic_write() {
  char dir[] = "/tmp/dsperate-safe-XXXXXX";
  CHECK(mkdtemp(dir));
  const std::string path = std::string(dir) + "/game.sav", bak = path + ".prestate.bak";
  std::string err;
  CHECK(write_file_atomic(path, "one", 3, err));
  CHECK(slurp(path) == "one");
  CHECK(write_file_atomic(path, "two", 3, err, bak));
  CHECK(slurp(path) == "two" && slurp(bak) == "one");
  CHECK(write_file_atomic(path, "three", 5, err, bak));
  CHECK(slurp(path) == "three" && slurp(bak) == "two");
  CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
  // No file yet and something to keep: nothing to keep, not an error.
  const std::string fresh = std::string(dir) + "/new.sav";
  CHECK(write_file_atomic(fresh, "x", 1, err, fresh + ".bak"));
  CHECK(access((fresh + ".bak").c_str(), F_OK) != 0);
  // A place that cannot be written: reported, the old file untouched.
  CHECK(!write_file_atomic(std::string(dir) + "/missing/game.sav", "x", 1, err));
  CHECK(!err.empty());
  CHECK(slurp(path) == "three");
  for (const std::string& f : {path, bak, fresh}) std::remove(f.c_str());
  rmdir(dir);
}

} // namespace

int main() {
  test_failed_load_puts_the_machine_back();
  test_load_is_not_a_save();
  test_atomic_write();
  std::puts("state_load: ok");
  return 0;
}
