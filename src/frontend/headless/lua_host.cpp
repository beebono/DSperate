// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/headless/lua_host.h"

#include "core/nds.h"
#include "core/state/state.h"
#include "core/cart/miniz/miniz_tdef.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ds::headless {

namespace {

// The registry key the host's pointer sits under.
const char kHostKey = 0;
// What ds.exit raises, so a pcall in the script can't swallow it unnoticed.
const char* const kExitMarker = "ds.exit";

// ds.setkeys names, in Io::Button order.
const char* const kButtons[] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};

std::vector<u8> slurp(const char* path) {
  std::vector<u8> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (n > 0) { v.resize(static_cast<size_t>(n)); if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); }
  std::fclose(f);
  return v;
}

} // namespace

struct LuaApi {
  static LuaHost& host(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kHostKey);
    auto* h = static_cast<LuaHost*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *h;
  }
  // The optional CPU argument at `idx`: "arm9" (default) or "arm7".
  static Cpu cpu_arg(lua_State* L, int idx) {
    const char* c = luaL_optstring(L, idx, "arm9");
    if (!std::strcmp(c, "arm9")) return Cpu::ARM9;
    if (!std::strcmp(c, "arm7")) return Cpu::ARM7;
    luaL_argerror(L, idx, "expected \"arm9\" or \"arm7\"");
    return Cpu::ARM9;
  }
  static u32 addr_arg(lua_State* L, int idx) { return static_cast<u32>(luaL_checkinteger(L, idx)); }

  // ds.frameadvance([n]): runs n frames (default 1) with the held keys and pen.
  static int frameadvance(lua_State* L) {
    LuaHost& h = host(L);
    const lua_Integer n = luaL_optinteger(L, 1, 1);
    for (lua_Integer k = 0; k < n; ++k) {
      h.nds_.io.set_buttons(h.keys_);
      h.nds_.io.set_touch(h.touch_x_, h.touch_y_, h.touch_down_);
      h.nds_.run_frame();
      if (h.after_frame) h.after_frame(h.nds_, h.after_frame_user);
    }
    return 0;
  }
  static int framecount(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(host(L).nds_.frame_count)); return 1; }

  // Reads and writes go through the DMA path: any address as that CPU sees it
  // (TCMs, I/O included), and a write invalidates translated code it hits.
  static int read8(lua_State* L) { lua_pushinteger(L, host(L).nds_.bus.dma_read8(cpu_arg(L, 2), addr_arg(L, 1))); return 1; }
  static int read16(lua_State* L) { lua_pushinteger(L, host(L).nds_.bus.dma_read16(cpu_arg(L, 2), addr_arg(L, 1))); return 1; }
  static int read32(lua_State* L) { lua_pushinteger(L, host(L).nds_.bus.dma_read32(cpu_arg(L, 2), addr_arg(L, 1))); return 1; }
  static int readrange(lua_State* L) {
    const u32 a = addr_arg(L, 1);
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_argcheck(L, n >= 0 && n <= (64 << 20), 2, "length out of range");
    const Cpu c = cpu_arg(L, 3);
    mem::Bus& bus = host(L).nds_.bus;
    luaL_Buffer b;
    char* p = luaL_buffinitsize(L, &b, static_cast<size_t>(n));
    for (lua_Integer k = 0; k < n; ++k) p[k] = static_cast<char>(bus.dma_read8(c, a + static_cast<u32>(k)));
    luaL_pushresultsize(&b, static_cast<size_t>(n));
    return 1;
  }
  static int write8(lua_State* L) { host(L).nds_.bus.dma_write8(cpu_arg(L, 3), addr_arg(L, 1), static_cast<u8>(luaL_checkinteger(L, 2))); return 0; }
  static int write16(lua_State* L) { host(L).nds_.bus.dma_write16(cpu_arg(L, 3), addr_arg(L, 1), static_cast<u16>(luaL_checkinteger(L, 2))); return 0; }
  static int write32(lua_State* L) { host(L).nds_.bus.dma_write32(cpu_arg(L, 3), addr_arg(L, 1), static_cast<u32>(luaL_checkinteger(L, 2))); return 0; }

  // ds.setkeys(mask | {"a", "right", ...} | {a = true, ...}): the buttons held
  // from the next frame on, until set again.
  static int setkeys(lua_State* L) {
    LuaHost& h = host(L);
    if (lua_isinteger(L, 1)) { h.keys_ = static_cast<unsigned>(lua_tointeger(L, 1)) & 0x0FFF; return 0; }
    luaL_checktype(L, 1, LUA_TTABLE);
    unsigned keys = 0;
    for (unsigned b = 0; b < 12; ++b) {
      lua_getfield(L, 1, kButtons[b]);
      if (lua_toboolean(L, -1)) keys |= 1u << b;
      lua_pop(L, 1);
    }
    for (lua_Integer i = 1;; ++i) {
      if (lua_rawgeti(L, 1, i) == LUA_TNIL) { lua_pop(L, 1); break; }
      const char* name = luaL_checkstring(L, -1);
      bool known = false;
      for (unsigned b = 0; b < 12; ++b) if (!std::strcmp(name, kButtons[b])) { keys |= 1u << b; known = true; }
      if (!known) return luaL_error(L, "ds.setkeys: no button \"%s\"", name);
      lua_pop(L, 1);
    }
    h.keys_ = keys;
    return 0;
  }
  // ds.touch(x, y) holds the pen on the bottom screen; ds.touch() lifts it.
  static int touch(lua_State* L) {
    LuaHost& h = host(L);
    if (lua_isnoneornil(L, 1)) { h.touch_down_ = false; return 0; }
    h.touch_x_ = static_cast<int>(luaL_checkinteger(L, 1));
    h.touch_y_ = static_cast<int>(luaL_checkinteger(L, 2));
    h.touch_down_ = true;
    return 0;
  }

  // ds.savestate(path) / ds.loadstate(path): true, or nil and a message.
  static int savestate(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    state::Writer w; std::string err;
    if (!host(L).nds_.save_state(w, err)) { lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2; }
    FILE* f = std::fopen(path, "wb");
    const bool ok = f && std::fwrite(w.data().data(), 1, w.data().size(), f) == w.data().size();
    if (f) std::fclose(f);
    if (!ok) { lua_pushnil(L); lua_pushfstring(L, "cannot write %s", path); return 2; }
    lua_pushboolean(L, 1);
    return 1;
  }
  static int loadstate(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::vector<u8> bytes = slurp(path);
    if (bytes.empty()) { lua_pushnil(L); lua_pushfstring(L, "cannot read %s", path); return 2; }
    state::Reader r(bytes.data(), bytes.size());
    std::string err;
    if (!host(L).nds_.load_state(r, err)) { lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2; }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ds.screenshot(path [, "top" | "bottom" | "both"]): the last frame as a
  // PNG; both screens stack top over bottom (256x384).
  static int screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const char* which = luaL_optstring(L, 2, "both");
    int first = 0, count = 2;
    if (!std::strcmp(which, "top")) count = 1;
    else if (!std::strcmp(which, "bottom")) { first = 1; count = 1; }
    else if (std::strcmp(which, "both")) return luaL_argerror(L, 2, "expected \"top\", \"bottom\" or \"both\"");
    const int w = static_cast<int>(SCREEN_W), h = static_cast<int>(SCREEN_H) * count;
    std::vector<u8> rgb(static_cast<size_t>(w) * h * 3);
    for (int s = 0; s < count; ++s) {
      const u32* fb = host(L).nds_.gpu.framebuffer(first + s);   // 0xAARRGGBB
      u8* dst = rgb.data() + static_cast<size_t>(s) * SCREEN_W * SCREEN_H * 3;
      for (u32 k = 0; k < SCREEN_W * SCREEN_H; ++k) {
        dst[k * 3 + 0] = static_cast<u8>(fb[k] >> 16);
        dst[k * 3 + 1] = static_cast<u8>(fb[k] >> 8);
        dst[k * 3 + 2] = static_cast<u8>(fb[k]);
      }
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), w, h, 3, &len, 6, MZ_FALSE);
    if (!png) { lua_pushnil(L); lua_pushstring(L, "png: cannot encode"); return 2; }
    FILE* f = std::fopen(path, "wb");
    const bool ok = f && std::fwrite(png, 1, len, f) == len;
    if (f) std::fclose(f);
    std::free(png);   // miniz allocates with plain malloc
    if (!ok) { lua_pushnil(L); lua_pushfstring(L, "cannot write %s", path); return 2; }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ds.exit([code]): ends the run with that exit code (default 0).
  static int exit(lua_State* L) {
    LuaHost& h = host(L);
    h.exit_code_ = static_cast<int>(luaL_optinteger(L, 1, 0));
    h.exiting_ = true;
    lua_pushstring(L, kExitMarker);
    return lua_error(L);
  }
};

LuaHost::LuaHost(NDS& nds) : nds_(nds) {
  L_ = luaL_newstate();
  luaL_openlibs(L_);
  lua_pushlightuserdata(L_, this);
  lua_rawsetp(L_, LUA_REGISTRYINDEX, &kHostKey);
  static const luaL_Reg api[] = {
    {"frameadvance", LuaApi::frameadvance}, {"framecount", LuaApi::framecount},
    {"read8", LuaApi::read8}, {"read16", LuaApi::read16}, {"read32", LuaApi::read32}, {"readrange", LuaApi::readrange},
    {"write8", LuaApi::write8}, {"write16", LuaApi::write16}, {"write32", LuaApi::write32},
    {"setkeys", LuaApi::setkeys}, {"touch", LuaApi::touch},
    {"savestate", LuaApi::savestate}, {"loadstate", LuaApi::loadstate},
    {"screenshot", LuaApi::screenshot}, {"exit", LuaApi::exit},
    {nullptr, nullptr},
  };
  luaL_newlib(L_, api);
  lua_setglobal(L_, "ds");
}

LuaHost::~LuaHost() { lua_close(L_); }

int LuaHost::run(int load_status) {
  if (load_status == LUA_OK) load_status = lua_pcall(L_, 0, 0, 0);
  if (load_status == LUA_OK) return 0;
  if (exiting_) { lua_pop(L_, 1); return exit_code_; }
  const char* msg = lua_tostring(L_, -1);
  error_ = msg ? msg : "(error object is not a string)";
  lua_pop(L_, 1);
  return 1;
}

int LuaHost::run_file(const std::string& path) {
  const int rc = run(luaL_loadfile(L_, path.c_str()));
  if (rc != 0 && !exiting_) std::fprintf(stderr, "lua: %s\n", error_.c_str());
  return rc;
}

int LuaHost::run_string(const std::string& source, const char* name) {
  return run(luaL_loadbuffer(L_, source.data(), source.size(), name));
}

} // namespace ds::headless
