// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// A save state of the whole machine, there and back: a machine that loads a
// state must go on exactly as the one that made it does (every frame the
// same, and the same state at the end), whichever machine it is loaded into.
// And a state with bytes changed must never take the machine down: it loads
// or it is refused, and the machine runs on either way.
//
// The program is the wireframe test (tools/homebrew/wiretest): the ARM9
// feeds the geometry engine every frame and waits on VCOUNT, so the CPU, the
// scheduler, the 3D engine, the CPU raster and the 2D composite all carry
// state across the save.
#include "core/nds.h"
#include "core/state/state.h"
#include "check.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ds;

namespace {

const u8 kRom[] = {
#include "wiretest_rom.inc"
};

std::unique_ptr<NDS> machine() {
  std::vector<u8> rom(0x20000, 0);
  std::memcpy(rom.data(), kRom, 0x200);
  u32 a9_off, a9_size, a7_off;
  std::memcpy(&a9_off, kRom + 0x20, 4); std::memcpy(&a9_size, kRom + 0x2C, 4); std::memcpy(&a7_off, kRom + 0x30, 4);
  CHECK(sizeof kRom == 0x200 + a9_size);
  std::memcpy(rom.data() + a9_off, kRom + 0x200, a9_size);
  const u32 spin = 0xEAFFFFFEu;   // b .
  std::memcpy(rom.data() + a7_off, &spin, 4);
  auto p = std::make_unique<NDS>();
  CHECK(p->load_rom_image(std::move(rom)));
  p->reset();
  p->setup_direct_boot();
  return p;
}

std::vector<u8> snapshot(NDS& nds) {
  state::Writer w; std::string err;
  CHECK(nds.save_state(w, err));
  return w.data();
}

u64 picture(const NDS& nds) {
  u64 h = 1469598103934665603ull;
  for (int s = 0; s < 2; ++s) {
    const u32* fb = nds.gpu.framebuffer(s);
    for (u32 i = 0; i < 256 * 192; ++i) h = (h ^ (fb[i] & 0xFFFFFFu)) * 1099511628211ull;
  }
  return h;
}

void test_round_trip() {
  auto a = machine();
  for (int i = 0; i < 20; ++i) a->run_frame();
  const std::vector<u8> at20 = snapshot(*a);
  std::vector<u64> want;
  for (int i = 0; i < 12; ++i) { a->run_frame(); want.push_back(picture(*a)); }
  const std::vector<u8> at32 = snapshot(*a);
  // The program draws, and the picture moves (a square slides): the frames are not all one.
  bool moved = false;
  for (u64 h : want) moved = moved || h != want[0];
  CHECK(moved);

  // Into the machine that made it (which has run on since), and into a fresh one.
  auto fresh = machine();
  for (NDS* m : {a.get(), fresh.get()}) {
    state::Reader r(at20.data(), at20.size());
    std::string err;
    CHECK(m->load_state(r, err));
    for (int i = 0; i < 12; ++i) { m->run_frame(); CHECK(picture(*m) == want[static_cast<size_t>(i)]); }
    CHECK(snapshot(*m) == at32);
  }
}

// Bytes changed here and there. Whatever a load says, the machine it was asked of runs on;
// a refused load leaves it exactly as it was.
void test_changed_bytes() {
  auto p = machine();
  NDS& nds = *p;
  for (int i = 0; i < 10; ++i) nds.run_frame();
  const std::vector<u8> good = snapshot(nds);
  u32 seed = 0x9E3779B9u;
  const auto rnd = [&seed] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
  // The file is a magic, a version and then chunks (tag, length, payload). Most of it is memory, where a changed
  // byte is only a different machine: half the changes go to a chunk's header and its first bytes, where the
  // counts, indices and lengths are.
  std::vector<size_t> chunks;
  for (size_t at = 8; at + 8 <= good.size();) {
    u32 len; std::memcpy(&len, good.data() + at + 4, 4);
    chunks.push_back(at);
    at += 8 + static_cast<size_t>(len);
  }
  CHECK(chunks.size() > 8);
  u32 refused = 0, taken = 0;
  for (int trial = 0; trial < 400; ++trial) {
    std::vector<u8> bad = good;
    const u32 n = 1 + rnd() % 6;
    for (u32 k = 0; k < n; ++k) {
      size_t at = rnd() % bad.size();
      if (rnd() & 1) { const size_t c = chunks[rnd() % chunks.size()]; at = std::min(c + rnd() % 520, bad.size() - 1); }
      switch (rnd() % 3) {
      case 0: bad[at] = static_cast<u8>(rnd()); break;
      case 1: bad[at] = 0xFF; if (at + 3 < bad.size()) { bad[at + 1] = bad[at + 2] = bad[at + 3] = 0xFF; } break;   // a count or a length gone wild
      default: bad[at] ^= static_cast<u8>(1u << (rnd() % 8)); break;
      }
    }
    const std::vector<u8> before = snapshot(nds);
    state::Reader r(bad.data(), bad.size());
    std::string err;
    if (nds.load_state(r, err)) ++taken;
    else {
      ++refused;
      const std::vector<u8> after = snapshot(nds);
      if (after != before) {
        size_t d = 0; while (d < after.size() && d < before.size() && after[d] == before[d]) ++d;
        size_t c = 0; for (size_t x : chunks) if (x <= d) c = x;
        u32 dn = 0; for (size_t i = 0; i < after.size() && i < before.size(); ++i) dn += after[i] != before[i];
        { u32 shown = 0; for (size_t i = 0; i < after.size() && shown < 24; ++i) if (after[i] != before[i]) { std::fprintf(stderr, "  +%zu: %02x -> %02x\n", i - c - 8, before[i], after[i]); ++shown; } }
        std::fprintf(stderr, "trial %d: refused (%s) but the machine differs: sizes %zu/%zu, first at %zu in chunk %.4s (+%zu), %u bytes differ\n", trial, err.c_str(), before.size(), after.size(), d, reinterpret_cast<const char*>(before.data() + c), d - c - 8, dn);
      }
      CHECK(after == before);
    }
    nds.run_frame();
    nds.run_frame();
  }
  std::printf("state_roundtrip: %u changed states refused, %u loaded\n", refused, taken);
  CHECK(refused > 0 && taken > 0);
  // And the good one still loads.
  state::Reader r(good.data(), good.size());
  std::string err;
  CHECK(nds.load_state(r, err));
  nds.run_frame();
}

} // namespace

int main() {
  test_round_trip();
  test_changed_bytes();
  std::puts("state_roundtrip: ok");
  return 0;
}
