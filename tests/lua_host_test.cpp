// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The headless --lua host (lua_host.h): memory reads and writes through
// either CPU, keys, frames, save and load states to a file, screenshots, the
// exit code, and errors, on a blank cartridge (no game needed).
#include "core/nds.h"
#include "frontend/headless/lua_host.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ds;
using headless::LuaHost;

namespace {

// Runs a chunk, printing the script's error when it fails.
int run(LuaHost& lua, const std::string& src) {
  const int rc = lua.run_string(src);
  if (rc != 0 && !lua.error().empty()) std::fprintf(stderr, "lua: %s\n", lua.error().c_str());
  return rc;
}

void test_memory() {
  NDS nds;
  LuaHost lua(nds);
  CHECK(lua.run_string(R"(
    ds.write32(0x02000100, 0xCAFEBABE)
    ds.write16(0x02000104, 0x1234)
    ds.write8(0x02000106, 0x56)
    assert(ds.read32(0x02000100) == 0xCAFEBABE)
    assert(ds.read16(0x02000104) == 0x1234)
    assert(ds.read8(0x02000106) == 0x56)
    assert(ds.read8(0x02000100, "arm7") == 0xBE)
    assert(ds.readrange(0x02000104, 3) == "\x34\x12\x56")
    assert(ds.framecount() == 0)
  )") == 0);
  CHECK(nds.bus.dma_read32(Cpu::ARM9, 0x02000100) == 0xCAFEBABE);
}

void test_keys() {
  NDS nds;
  LuaHost lua(nds);
  CHECK(lua.run_string("ds.setkeys({'a', 'right'})") == 0);
  CHECK(lua.keys() == ((1u << io::Io::BTN_A) | (1u << io::Io::BTN_RIGHT)));
  CHECK(lua.run_string("ds.setkeys({start = true, y = true})") == 0);
  CHECK(lua.keys() == ((1u << io::Io::BTN_START) | (1u << io::Io::BTN_Y)));
  CHECK(lua.run_string("ds.setkeys(0)") == 0);
  CHECK(lua.keys() == 0);
  CHECK(lua.run_string("ds.setkeys({'jump'})") == 1);
  CHECK(lua.error().find("no button \"jump\"") != std::string::npos);
}

void test_frames_and_states() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  CHECK(nds.load_rom_image(std::vector<u8>(0x4000)));   // a state needs a cartridge
  nds.reset();
  LuaHost lua(nds);
  const std::string dir = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
  const std::string state = dir + "/dsperate_lua_host_test.dss", png = dir + "/dsperate_lua_host_test.png";
  int frames = 0;
  lua.after_frame = [](NDS&, void* u) { ++*static_cast<int*>(u); };
  lua.after_frame_user = &frames;
  const std::string src = "local state, png = '" + state + "', '" + png + "'\n" + R"(
    assert(ds.framecount() == 0)
    ds.setkeys({'a'})
    ds.frameadvance(2)
    assert(ds.framecount() == 2)
    ds.write32(0x02000200, 0x11111111)
    assert(ds.savestate(state))
    ds.frameadvance()
    ds.write32(0x02000200, 0x22222222)
    assert(ds.loadstate(state))
    assert(ds.read32(0x02000200) == 0x11111111)
    assert(ds.framecount() == 2)
    local ok, err = ds.loadstate(state .. '.missing')
    assert(ok == nil and err:find('cannot read'))
    assert(ds.screenshot(png, 'top'))
  )";
  CHECK(run(lua, src) == 0);
  CHECK(frames == 3);
  CHECK((nds.io.keyinput & 1) == 0);   // KEYINPUT: A held (active low)
  FILE* f = std::fopen(png.c_str(), "rb");
  CHECK(f != nullptr);
  unsigned char head[8] = {};
  CHECK(std::fread(head, 1, 8, f) == 8 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G');
  std::fclose(f);
  std::remove(state.c_str());
  std::remove(png.c_str());
}

void test_exit_and_errors() {
  NDS nds;
  {
    LuaHost lua(nds);
    CHECK(lua.run_string("ds.exit(3) error('not reached')") == 3);
  }
  {
    LuaHost lua(nds);
    CHECK(lua.run_string("ds.read8(0, 'arm11')") == 1);
    CHECK(lua.error().find("arm7") != std::string::npos);
  }
  {
    LuaHost lua(nds);
    CHECK(lua.run_string("this is not lua") == 1);
  }
}

} // namespace

int main() {
  test_memory();
  test_keys();
  test_frames_and_states();
  test_exit_and_errors();
  std::puts("lua_host: ok");
  return 0;
}
