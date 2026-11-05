// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The headless harness's Lua mode (--lua SCRIPT): the script owns the frame
// loop and drives the machine through a `ds` table -- run a frame, read and
// write guest memory as either CPU sees it, hold buttons and the pen, save
// and load states to files, write PNG screenshots. Made for closed-loop
// tools (bots, probes, tracers) that decide each frame from what they read.
// See docs/headless-scripting.md for the API.
#pragma once
#include <string>

struct lua_State;
namespace ds { struct NDS; }
namespace ds::headless {

class LuaHost {
public:
  explicit LuaHost(NDS& nds);
  ~LuaHost();
  LuaHost(const LuaHost&) = delete;
  LuaHost& operator=(const LuaHost&) = delete;

  // Runs a script file (or a chunk of source, for tests). Returns the exit
  // code: ds.exit(n)'s n, 0 when the script returns, 1 on a Lua error
  // (printed to stderr, or kept in error() for run_string).
  int run_file(const std::string& path);
  int run_string(const std::string& source, const char* name = "=lua");
  const std::string& error() const { return error_; }

  // Called after every frame the script runs (the read log's frame window
  // uses it); null for none.
  void (*after_frame)(NDS& nds, void* user) = nullptr;
  void* after_frame_user = nullptr;

  // The pad and pen the next frame starts with (Io::Button bits).
  unsigned keys() const { return keys_; }

private:
  friend struct LuaApi;
  int run(int load_status);
  NDS& nds_;
  lua_State* L_ = nullptr;
  unsigned keys_ = 0;
  int touch_x_ = 0, touch_y_ = 0;
  bool touch_down_ = false;
  bool exiting_ = false;
  int exit_code_ = 0;
  std::string error_;
};

} // namespace ds::headless
