// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SDL2 frontend: direct boot, both screens stacked, sound, and input from a
// keyboard, a game controller or a touchscreen. Settings come from an INI
// file (config.h) with the command line on top; hotkeys cover what a
// handheld needs (pause, volume, layout, screenshots, save states). No menus.
#include "core/nds.h"
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/input/input_log.h"
#include "core/state/state.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "lid.h"
#include "mic_alsa.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using namespace ds;

const char* kUsage =
    "usage: dsperate-sdl <rom.nds> [--bios9 F --bios7 F --firmware F] [options]\n"
    "  --config F      settings file (default ~/.config/dsperate/dsperate.ini; every\n"
    "                  option below has a key there; games/<rom name>.ini and games/<CODE>.ini\n"
    "                  override it per game, the filename one winning)\n"
    "  --write-config F  write the default settings file (all keys commented) to F and exit\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) | horizontal | single | pip | dominant_v | dominant_h\n"
    "  --screen S      top (default) or bottom: the screen shown alone, large or dominant\n"
    "  --dual-window   one window per video display, one DS screen each (dual-panel\n"
    "                  handhelds; also what direct scanout needs on them)\n"
    "  --linear        smooth scaling instead of nearest\n"
    "  --lcd-grid S    LCD pixel grid strength, 0 (off, default) .. 1 (software scaling only)\n"
    "  --seam S        dark (default): the LCD grid, dimmed by --lcd-grid | blend: box-filter seams\n"
    "                  (sharp-shimmerless): the one panel pixel/row that straddles two DS pixels is their\n"
    "                  area-weighted blend, all others crisp | blend_linear: the same in linear light\n"
    "  --chunky [M]    draw each 2x2 block of DS pixels as one cell (with the grid, one seam per\n"
    "                  block); lower resolution, for panels at fractional scales. M: mean (default;\n"
    "                  the area-weighted box) | extreme (the mean, unless the darkest or brightest\n"
    "                  pixel stands more than --chunky-threshold N (0..255, default 180) from it) |\n"
    "                  mode (dominant colour, mean when all differ) | tl (top-left pixel) | min | max\n"
    "  --chunky-cell C panel pixels per chunky cell: auto (default; the smallest of 4..16 that divides the\n"
    "                  screen, 4 on a 640x480 panel = 160x120 cells) | pair (2x2 DS pixels) | N\n"
    "  --accel         GPU renderer; the default is software, which measures faster\n"
    "                  on the handhelds (the GL driver's threads cost more than the scale)\n"
    "  --no-audio      run without sound (frames are paced by the clock)\n"
    "  --volume N      0..100\n"
    "  --no-mic        do not open the microphone (M still fakes one)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --lockstep      128-cycle CPU interleave (melonDS lockstep) instead of event-bound; --quantum N for any value\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n"
    "  --save F        battery save to start from, instead of <rom>.sav\n"
    "                  (a --replay never writes the save back, so a scene repeats)\n";

std::string rom_stem(const std::string& rom) {
  const size_t dot = rom.find_last_of('.');
  const size_t slash = rom.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return rom;
  return rom.substr(0, dot);
}
std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Battery save: next to the ROM, or under [paths] saves.
std::string save_path(const std::string& rom, const std::string& dir) {
  return dir.empty() ? rom_stem(rom) + ".sav" : dir + "/" + base_name(rom_stem(rom)) + ".sav";
}

void load_save(NDS& nds, const std::string& path) {
  if (!nds.cart || nds.cart->sram().empty()) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  std::vector<u8>& sram = nds.cart->sram();
  const size_t n = std::fread(sram.data(), 1, sram.size(), f);
  std::fclose(f);
  std::fprintf(stderr, "save: loaded %zu bytes from %s\n", n, path.c_str());
}

void write_save(NDS& nds, const std::string& path) {
  if (!nds.cart || nds.cart->sram().empty()) return;
  // Write-then-rename so a power cut mid-write leaves the previous file.
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "save: cannot write %s\n", tmp.c_str()); return; }
  const bool ok = std::fwrite(nds.cart->sram().data(), 1, nds.cart->sram().size(), f) == nds.cart->sram().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "save: cannot write %s\n", path.c_str()); return; }
  nds.cart->clear_sram_dirty();
}

// Both screens into one BMP, stacked or side by side like the window.
void screenshot(NDS& nds, const std::string& dir, bool across) {
  const int w = across ? 512 : 256, h = across ? 192 : 384;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return;
  for (int i = 0; i < 2; ++i) {
    const u32* fb = nds.gpu.framebuffer(i);
    for (int y = 0; y < 192; ++y) {
      u32* dst = reinterpret_cast<u32*>(static_cast<u8*>(s->pixels) + (across ? y : y + 192 * i) * s->pitch) + (across ? 256 * i : 0);
      std::memcpy(dst, fb + y * 256, 256 * 4);
    }
  }
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  const std::string path = dir + "/" + code + "-" + stamp + ".bmp";
  if (SDL_SaveBMP(s, path.c_str()) == 0) std::fprintf(stderr, "screenshot: %s\n", path.c_str());
  else std::fprintf(stderr, "screenshot: %s\n", SDL_GetError());
  SDL_FreeSurface(s);
}

std::string state_path(NDS& nds, const std::string& dir, int slot) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + "." + std::to_string(slot) + ".dss";
}

bool save_state_file(NDS& nds, const std::string& path) {
  ds::state::Writer w; std::string err;
  if (!nds.save_state(w, err)) { std::fprintf(stderr, "state: cannot save: %s\n", err.c_str()); return false; }
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "state: cannot write %s\n", tmp.c_str()); return false; }
  const bool ok = std::fwrite(w.data().data(), 1, w.data().size(), f) == w.data().size();
  std::fclose(f);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "state: cannot write %s\n", path.c_str()); return false; }
  std::fprintf(stderr, "state: saved %s (%zu KB)\n", path.c_str(), w.data().size() >> 10);
  return true;
}

// False when the file is unusable and the machine was left alone; the
// caller must reset the machine if this fails after the load began (the
// error says so).
bool load_state_file(NDS& nds, const std::string& path) {
  std::vector<u8> bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n > 0) { bytes.resize(static_cast<size_t>(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
    std::fclose(f);
  }
  if (bytes.empty()) { std::fprintf(stderr, "state: no state in slot (%s)\n", path.c_str()); return false; }
  ds::state::Reader r(bytes.data(), bytes.size());
  std::string err;
  if (!nds.load_state(r, err)) { std::fprintf(stderr, "state: cannot load %s: %s\n", path.c_str(), err.c_str()); return false; }
  std::fprintf(stderr, "state: loaded %s (frame %llu)\n", path.c_str(), static_cast<unsigned long long>(nds.frame_count));
  return true;
}

// The stick-driven pen: an outlined crosshair with a red centre, drawn over
// the bottom screen in DS pixel space and mapped onto the destination (the
// frontend's scaled buffer, or a copy of the framebuffer).
struct CursorDst { u32* px; u32 pitch; u32 h; const u16* xrun; };   // xrun null: 1:1
void draw_cursor(const CursorDst& d, int cx, int cy, int size) {
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x > 255 || y < 0 || y > 191) return;
    const u32 x0 = d.xrun ? d.xrun[x] : static_cast<u32>(x), x1 = d.xrun ? d.xrun[x + 1] : static_cast<u32>(x + 1);
    const u32 y0 = d.h * static_cast<u32>(y) / 192, y1 = d.h * static_cast<u32>(y + 1) / 192;
    for (u32 yy = y0; yy < y1; ++yy) for (u32 xx = x0; xx < x1; ++xx) d.px[yy * d.pitch + xx] = colour;
  };
  // `size` scales the whole shape: arms `size` wide and 3*size long, a
  // size x size red centre where they meet, a one-pixel black outline.
  auto box = [&](int x0, int y0, int w, int h, u32 colour) { for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) fill(x, y, colour); };
  const u32 outline = 0xFF000000, line = 0xFFFFFFFF, centre = 0xFFFF2020;
  if (size < 1) size = 1;
  const int arm = 3 * size, half = size / 2;          // the centre box spans [cx-half, cx-half+size)
  const int c0 = -half, c1 = -half + size;            // centre extent relative to cx/cy
  box(cx + c0 - arm - 1, cy + c0 - 1, 2 * arm + size + 2, size + 2, outline);   // horizontal bar outline
  box(cx + c0 - 1, cy + c0 - arm - 1, size + 2, 2 * arm + size + 2, outline);   // vertical bar outline
  box(cx + c0 - arm, cy + c0, 2 * arm + size, size, line);
  box(cx + c0, cy + c0 - arm, size, 2 * arm + size, line);
  box(cx + c0, cy + c0, c1 - c0, c1 - c0, centre);
}

// The state slot, shown briefly after a slot hotkey: a white digit (3x5
// font, doubled) on a black box in the top-left corner of the top screen.
void draw_slot(const CursorDst& d, int digit) {
  static const u8 font[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}};
  auto fill = [&](int x, int y, u32 colour) {
    const u32 x0 = d.xrun ? d.xrun[x] : static_cast<u32>(x), x1 = d.xrun ? d.xrun[x + 1] : static_cast<u32>(x + 1);
    const u32 y0 = d.h * static_cast<u32>(y) / 192, y1 = d.h * static_cast<u32>(y + 1) / 192;
    for (u32 yy = y0; yy < y1; ++yy) for (u32 xx = x0; xx < x1; ++xx) d.px[yy * d.pitch + xx] = colour;
  };
  const int S = 2, X = 4, Y = 4;                     // glyph scale and box origin
  for (int y = 0; y < 5 * S + 4; ++y) for (int x = 0; x < 3 * S + 4; ++x) fill(X + x, Y + y, 0xFF000000);
  for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
    if ((font[digit][r] >> (2 - c)) & 1)
      for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) fill(X + 2 + c * S + x, Y + 2 + r * S + y, 0xFFFFFFFF);
}

// A launcher's SIGTERM (or Ctrl-C) must still flush the battery save.
volatile std::sig_atomic_t g_signalled = 0;
void on_signal(int) { g_signalled = 1; }

} // namespace

int main(int argc, char** argv) {
  const char* rom = nullptr;
  const char* config_arg = nullptr;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr, *save_arg = nullptr;

  // The command line is one more settings layer, applied after the files.
  ds::sdl::Config cli;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    if (arg("--bios9")) cli.set("paths.bios9", argv[++i]);
    else if (arg("--bios7")) cli.set("paths.bios7", argv[++i]);
    else if (arg("--firmware")) cli.set("paths.firmware", argv[++i]);
    else if (arg("--config")) config_arg = argv[++i];
    else if (arg("--write-config")) { ds::sdl::Config::write_default(argv[++i], true); return 0; }
    else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--screen")) cli.set("video.screen", argv[++i]);
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (flag("--fullscreen")) cli.set("video.fullscreen", "true");
    else if (flag("--linear")) cli.set("video.linear", "true");
    else if (arg("--lcd-grid")) cli.set("video.lcd_grid", argv[++i]);
    else if (flag("--chunky")) cli.set("video.chunky", i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "mean");
    else if (arg("--chunky-threshold")) cli.set("video.chunky_threshold", argv[++i]);
    else if (arg("--chunky-cell")) cli.set("video.chunky_cell", argv[++i]);
    else if (arg("--seam")) cli.set("video.seam", argv[++i]);
    else if (flag("--accel")) cli.set("video.accel", "true");
    else if (flag("--no-audio")) cli.set("audio.enabled", "false");
    else if (arg("--volume")) cli.set("audio.volume", argv[++i]);
    else if (flag("--no-mic")) cli.set("audio.mic", "false");
    else if (flag("--no-vsync")) cli.set("video.vsync", "false");
    else if (flag("--interp")) cli.set("emu.jit", "false");
    else if (flag("--lockstep")) cli.set("emu.quantum", std::to_string(ds::LOCKSTEP_QUANTUM));
    else if (arg("--quantum")) cli.set("emu.quantum", argv[++i]);
    else if (flag("--help")) { std::fputs(kUsage, stderr); return 0; }
    else if (argv[i][0] == '-' && argv[i][1] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); std::fputs(kUsage, stderr); return 2; }
    else rom = argv[i];
  }
  if (!rom) { std::fputs(kUsage, stderr); return 2; }

  ds::sdl::Config cfg;
  const std::string global_ini = config_arg ? std::string(config_arg) : ds::sdl::Config::global_path();
  if (!config_arg) ds::sdl::Config::write_default(global_ini);
  if (!cfg.load(global_ini) && config_arg) { std::fprintf(stderr, "cannot read %s\n", config_arg); return 2; }
  auto apply_cli = [&] { for (const char* k : {"paths.bios9", "paths.bios7", "paths.firmware", "video.scale", "video.dual_window", "video.layout", "video.screen",
                                              "video.fullscreen", "video.linear", "video.lcd_grid", "video.chunky", "video.chunky_threshold", "video.chunky_cell", "video.seam", "video.accel", "video.vsync", "audio.enabled", "audio.volume",
                                              "audio.mic", "emu.jit", "emu.quantum"}) if (cli.has(k)) cfg.set(k, cli.str(k)); };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7"), fw = cfg.str("paths.firmware");
  if (bios9.empty() || bios7.empty() || fw.empty()) { std::fprintf(stderr, "BIOS and firmware paths are needed (--bios9/--bios7/--firmware or [paths] in %s)\n", global_ini.c_str()); return 2; }

  NDS nds;
  if (!nds.load_bios(bios9.c_str(), bios7.c_str(), fw.c_str())) { std::fprintf(stderr, "could not load BIOS/firmware\n"); return 1; }
  nds.reset();
  if (!nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  // The per-game file goes on top of the global one, the command line on top of both.
  // Title ID first, then the ROM's filename, so the file named like the ROM
  // wins; that is also where hotkey-picked settings are remembered.
  std::string game_ini;
  if (nds.cart) {
    for (const std::string& p : {ds::sdl::Config::game_path_code(nds.cart->header().game_code), ds::sdl::Config::game_path_rom(rom)})
      if (!p.empty() && cfg.load(p)) std::fprintf(stderr, "config: %s\n", p.c_str());
    game_ini = ds::sdl::Config::game_path_rom(rom);
    if (game_ini.empty()) game_ini = ds::sdl::Config::game_path_code(nds.cart->header().game_code);
    apply_cli();
    std::fprintf(stderr, "game: %.12s [%.4s]\n", nds.cart->header().game_title, nds.cart->header().game_code);
  }
  // Core knobs that the core reads from the environment.
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP")) setenv("DS_IDLE_SKIP", cfg.str("emu.idle_skip").c_str(), 1);

  int scale = cfg.num("video.scale", 2);
  if (scale < 1) scale = 1;
  const bool fullscreen = cfg.flag("video.fullscreen", false), linear = cfg.flag("video.linear", false), accel = cfg.flag("video.accel", false);
  // Grid strength -> brightness kept on the seams, 0..256 (256 = off).
  const double grid_s = std::min(1.0, std::max(0.0, cfg.real("video.lcd_grid", 0.0)));
  const u32 grid = static_cast<u32>(std::lround((1.0 - grid_s) * 256.0));
  u8 seam_blend = 0;
  {
    const std::string sm = cfg.str("video.seam", "dark");
    if (sm == "blend") seam_blend = 1; else if (sm == "blend_linear") seam_blend = 2;
    else if (sm != "dark") { std::fprintf(stderr, "unknown seam %s (dark | blend | blend_linear)\n", sm.c_str()); return 2; }
  }
  u8 chunky = 0;
  int chunky_cell = -1;
  {
    const std::string c = cfg.str("video.chunky_cell", "auto");
    if (c == "auto") chunky_cell = -1; else if (c == "pair" || c == "2x") chunky_cell = 0;
    else { chunky_cell = std::atoi(c.c_str()); if (chunky_cell < 2 || chunky_cell > 64) { std::fprintf(stderr, "chunky_cell %s: auto | pair | 2..64\n", c.c_str()); return 2; } }
  }
  const u32 chunky_thresh = static_cast<u32>(std::min(255, std::max(0, cfg.num("video.chunky_threshold", 180)))) * 256;
  {
    const std::string c = cfg.str("video.chunky", "false");
    if (c == "tl") chunky = 1; else if (c == "mean" || c == "true" || c == "1" || c == "yes" || c == "on") chunky = 2;
    else if (c == "min") chunky = 4; else if (c == "max") chunky = 5; else if (c == "mode") chunky = 3;
    else if (c == "extreme") chunky = 6;
    else if (!(c == "false" || c == "0" || c == "no" || c == "off" || c.empty())) { std::fprintf(stderr, "unknown chunky %s (mean | extreme | mode | tl | min | max | false)\n", c.c_str()); return 2; }
  }
  bool audio_on = cfg.flag("audio.enabled", true), mic_on = cfg.flag("audio.mic", true);
  const bool jit = cfg.flag("emu.jit", true), vsync = cfg.flag("video.vsync", true), dual_window = cfg.flag("video.dual_window", false);
  const long quantum = cfg.num("emu.quantum", 0);   // event-bound interleave (DraStic's rule): 5-10 % faster than lockstep
  using Disp = ds::sdl::Display;
  Disp::Layout layout;
  std::vector<Disp::Mode> layout_cycle;
  {
    const std::string l = cfg.str("video.layout", "vertical"), sc = cfg.str("video.screen", "top"), co = cfg.str("video.pip_corner", "br");
    if (!Disp::parse_mode(l, layout.mode)) { std::fprintf(stderr, "unknown layout %s\n", l.c_str()); return 2; }
    if (sc == "top") layout.primary = 0; else if (sc == "bottom") layout.primary = 1;
    else { std::fprintf(stderr, "unknown screen %s (top | bottom)\n", sc.c_str()); return 2; }
    if (!Disp::parse_corner(co, layout.corner)) { std::fprintf(stderr, "unknown pip_corner %s (tl | tr | bl | br)\n", co.c_str()); return 2; }
    // The ring layout_next/prev step through; a mode outside it joins at its start.
    std::string cyc = cfg.str("video.layout_cycle", "vertical,horizontal,single,pip,dominant_v,dominant_h");
    for (size_t at = 0; at <= cyc.size();) {
      size_t end = cyc.find(',', at); if (end == std::string::npos) end = cyc.size();
      std::string name = cyc.substr(at, end - at);
      name.erase(0, name.find_first_not_of(' ')); name.erase(name.find_last_not_of(' ') + 1);
      Disp::Mode m;
      if (!name.empty() && !Disp::parse_mode(name, m)) { std::fprintf(stderr, "unknown layout %s in layout_cycle\n", name.c_str()); return 2; }
      if (!name.empty()) layout_cycle.push_back(m);
      at = end + 1;
    }
    if (layout_cycle.empty()) layout_cycle.push_back(layout.mode);
    layout.pip = std::clamp(cfg.real("video.pip_scale", 1.0 / 3.0), 0.1, 0.9);
    layout.dominant = std::clamp(cfg.real("video.dominant_ratio", 0.5), 0.1, 0.99);
  }
  const std::string saves_dir = cfg.str("paths.saves");
  const std::string rom_dir = std::string(rom).find_last_of('/') == std::string::npos ? "." : std::string(rom).substr(0, std::string(rom).find_last_of('/'));
  const std::string states_dir = cfg.str("paths.states", rom_dir);   // states and screenshots

  nds.sched.set_quantum(quantum);
  nds.setup_direct_boot();
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
#else
  (void)jit;
#endif
  // A replay is a measurement, not a play session: it must start from the
  // same battery save every time or it is not reproducible, and writing back
  // would mean the second run of a scene no longer matches the first. The CLI
  // has always loaded --save read-only for this reason; match it here, and
  // take an explicit --save too so both frontends can be pointed at the same
  // scene save rather than one silently picking up <rom>.sav.
  const std::string sav = save_arg ? std::string(save_arg) : save_path(rom, saves_dir);
  load_save(nds, sav);
  const bool save_readonly = replay != nullptr;
  if (save_readonly) std::fprintf(stderr, "save: read-only for the replay\n");

  ds::input::Log log;
  if (record && replay) { std::fprintf(stderr, "--record and --replay are exclusive\n"); return 2; }
  if (record && !log.open_write(record)) { std::fprintf(stderr, "cannot write %s\n", record); return 1; }
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    std::fprintf(stderr, "replay: %u frames from %s\n", log.frames(), replay);
  }
  ds::prof::enabled = std::getenv("DS_PROFILE") != nullptr;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  u32 init = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (audio_on || mic_on) init |= SDL_INIT_AUDIO;
  if (SDL_Init(init) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    if (!(init & SDL_INIT_AUDIO) || SDL_Init(init & ~SDL_INIT_AUDIO) != 0) return 1;
    audio_on = mic_on = false;   // no audio subsystem: run silent
  }

  ds::sdl::Display display;
  ds::sdl::Display display2;   // dual-window: the bottom screen's own window
  int bottom_display = 1;
  if (dual_window) {
    if (SDL_GetNumVideoDisplays() < 2) { std::fprintf(stderr, "--dual-window needs two video displays\n"); SDL_Quit(); return 1; }
    // Which display is the physical bottom panel depends on the driver, both
    // verified on the dual-panel board: under KMSDRM display 0 is DSI-1,
    // which is -- unintuitively -- the lower panel, while sway's canvas
    // arranges the outputs the other way around.
    const char* vd = SDL_GetCurrentVideoDriver();
    bottom_display = vd && !std::strcmp(vd, "KMSDRM") ? 0 : 1;
    display.set_chunky(chunky != 0, chunky_cell); display2.set_chunky(chunky != 0, chunky_cell);
    if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout, accel, 0, 1 - bottom_display) ||
        !display2.open("DSperate (bottom)", scale, fullscreen, linear, vsync, layout, accel, 1, bottom_display)) { SDL_Quit(); return 1; }
    if (display.scaling() != display2.scaling()) { std::fprintf(stderr, "dual-window: mixed display modes\n"); SDL_Quit(); return 1; }
  } else { display.set_chunky(chunky != 0, chunky_cell); if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout, accel)) { SDL_Quit(); return 1; } }

  ds::sdl::Audio audio;
  if (audio_on) audio.open();
  audio.set_volume(cfg.num("audio.volume", 100));
  // Not during a replay: the log carries the mic, and an open capture device
  // would only add work to a measurement.
  ds::sdl::MicAlsa mic_alsa;
  if (mic_on && !replay && !mic_alsa.open(ds::spu::Spu::SAMPLE_RATE, cfg.str("audio.mic_dev").c_str())) audio.open_capture();

  ds::sdl::Input input;
  input.configure(cfg);
  input.open_controllers();
  ds::sdl::Lid lid;
  if (!replay) lid.open();
  std::vector<s16> mic, mic_raw, mic_queue;
  // The codec's ADC sits well off zero (the RG DS: ~4200 of DC), which at
  // the game's x80 would read as a constant shout; one-pole DC blocker,
  // ~25 Hz at 32768 Hz. DS_MIC_GAIN scales what is left (default 0.25: the
  // codec is hot, and Mario & Luigi's mic-test meter sits right at 0.25 / gate 5).
  double dc = 0.0;
  // Noise gate: the ADC's hiss (RG DS: ~500 rms after the DC block) is
  // still x80 louder than a DS's own floor, and games wait for quiet
  // before they listen. The floor is the slowest-rising rms seen; a frame
  // under DS_MIC_GATE times it (default 5, 0 = off) is sent as silence.
  const double mic_gate = std::getenv("DS_MIC_GATE") ? std::atof(std::getenv("DS_MIC_GATE")) : cfg.real("audio.mic_gate", 5.0);
  double mic_floor = 1e9;
  const double mic_gain = std::getenv("DS_MIC_GAIN") ? std::atof(std::getenv("DS_MIC_GAIN")) : cfg.real("audio.mic_gain", 0.25);
  const size_t mic_per_frame = ds::spu::Spu::SAMPLE_RATE * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;   // 547

  // Wall-clock pacing when there is no audio queue to pace against.
  const double frame_ns = 1e9 * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;
  Uint64 next_frame = SDL_GetPerformanceCounter();
  const double ticks_per_ns = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;

  const bool show_fps = std::getenv("DS_FPS") != nullptr;
  Uint64 fps_mark = SDL_GetPerformanceCounter();
  Uint64 emu_ticks = 0, draw_ticks = 0;
  u64 frames = 0;
  // Per-frame emulation time, for the same report the CLI prints. Only the
  // run_frame() slice goes in: the present blocks on vsync and audio.pace()
  // sleeps, and either one would peg every frame at the refresh interval and
  // hide exactly the clusters this is here to find.
  const double ticks_to_ms = 1e3 / static_cast<double>(SDL_GetPerformanceFrequency());
  std::vector<double> frame_ms, work_ms;   // emu slice; emu + present slice
  if (frame_limit > 0) { frame_ms.reserve(static_cast<size_t>(frame_limit)); work_ms.reserve(static_cast<size_t>(frame_limit)); }
  Uint64 pace_ticks = 0, draw_ticks_total = 0;
  bool paused = false;
  int state_slot = 0;
  // Fast forward: the `fast_forward` hotkey while held, or the toggle (also
  // [emu] fast_forward = true to start that way). ff_speed caps it as a
  // multiple of real time (0 = as fast as the machine goes); ff_skip presents
  // one frame in ff_skip+1 -- every frame is still emulated (the display
  // capture and VRAM feedback keep the run exact), only its scaling and
  // present are skipped.
  bool ff_toggle = cfg.flag("emu.fast_forward", false);
  const int ff_speed = cfg.num("emu.ff_speed", 0), ff_skip = cfg.num("emu.ff_skip", 3);
  bool was_fast = false;
  std::vector<u32> cursor_fb(ds::SCREEN_W * ds::SCREEN_H);   // bottom screen with the pen crosshair
  std::vector<u32> osd_fb(ds::SCREEN_W * ds::SCREEN_H);      // top screen with the slot digit
  int slot_shown = 0;                                        // frames left to show the slot digit
  // Battery save flush: once the chip has been quiet for a second, and at
  // every point a session could end (pause, lid, quit).
  u32 sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
  u64 sram_quiet_since = 0;
  auto flush_save = [&] { if (!save_readonly && nds.cart && nds.cart->sram_dirty()) write_save(nds, sav); };
  auto set_paused = [&](bool p) {
    if (p == paused) return;
    paused = p;
    audio.pause(p);
    if (p) flush_save(); else next_frame = SDL_GetPerformanceCounter();
    std::fprintf(stderr, "%s\n", p ? "paused" : "resumed");
  };
  while (!input.quit() && !g_signalled && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
    for (ds::sdl::Action a : input.take_actions()) {
      using A = ds::sdl::Action;
      switch (a) {
      case A::Pause: set_paused(!paused); break;
      case A::VolumeUp: audio.set_volume(audio.volume() + 10); audio.set_muted(false); std::fprintf(stderr, "volume %d%%\n", audio.volume()); break;
      case A::VolumeDown: audio.set_volume(audio.volume() - 10); std::fprintf(stderr, "volume %d%%\n", audio.volume()); break;
      case A::Mute: audio.set_muted(!audio.muted()); std::fprintf(stderr, "%s\n", audio.muted() ? "muted" : "unmuted"); break;
      case A::Fullscreen: display.toggle_fullscreen(); if (dual_window) display2.toggle_fullscreen(); break;
      case A::LayoutNext: case A::LayoutPrev: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        const int n = static_cast<int>(layout_cycle.size());
        int at = 0;
        for (int i = 0; i < n; ++i) if (layout_cycle[static_cast<size_t>(i)] == l.mode) { at = a == A::LayoutNext ? (i + 1) % n : (i + n - 1) % n; break; }
        l.mode = layout_cycle[static_cast<size_t>(at)];
        display.set_layout(l);
        std::fprintf(stderr, "layout: %s\n", Disp::mode_name(l.mode));
        if (!game_ini.empty()) ds::sdl::Config::store(game_ini, "video.layout", Disp::mode_name(l.mode));
        break;
      }
      case A::ScreenSwap: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.primary = 1 - l.primary;
        display.set_layout(l);
        if (!game_ini.empty()) ds::sdl::Config::store(game_ini, "video.screen", l.primary ? "bottom" : "top");
        break;
      }
      case A::PipCornerNext: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.corner = static_cast<Disp::Corner>((static_cast<int>(l.corner) + 1) % static_cast<int>(Disp::Corner::Count));
        display.set_layout(l);
        if (!game_ini.empty()) ds::sdl::Config::store(game_ini, "video.pip_corner", Disp::corner_name(l.corner));
        break;
      }
      case A::Screenshot: screenshot(nds, states_dir, display.across()); break;
      case A::Lid: input.set_lid(!input.lid()); std::fprintf(stderr, "lid: %s\n", input.lid() ? "closed" : "open"); if (input.lid()) flush_save(); break;
      case A::SlotNext: state_slot = (state_slot + 1) % 10; slot_shown = 90; std::fprintf(stderr, "state slot %d\n", state_slot); break;
      case A::SlotPrev: state_slot = (state_slot + 9) % 10; slot_shown = 90; std::fprintf(stderr, "state slot %d\n", state_slot); break;
      case A::SaveState:
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (save_state_file(nds, state_path(nds, states_dir, state_slot))) flush_save();   // the .sav and the state never diverge
        break;
      case A::LoadState:
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        // A recording is the inputs from boot; a load would leave it unreplayable.
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        if (load_state_file(nds, state_path(nds, states_dir, state_slot))) {
          audio.clear();
          next_frame = SDL_GetPerformanceCounter();
          flush_save();
        }
        break;
      case A::FastForwardToggle: ff_toggle = !ff_toggle; std::fprintf(stderr, "fast forward %s\n", ff_toggle ? "on" : "off"); break;
      default: break;
      }
    }
    if (paused) { SDL_Delay(10); continue; }
    input.update_stylus();
    ds::input::Frame in = input.frame();
    if (log.reading()) {
      if (!log.read(in)) break;   // the controls still quit; the log ends the run
      ds::input::apply(nds, in);
    } else {
      bool closed;
      if (lid.poll(closed)) { input.set_lid(closed); if (closed) flush_save(); }
      in.lid = input.lid();
      if (input.fake_mic()) input.fake_mic_frame(mic);
      else {
        if (mic_alsa.active()) mic_alsa.capture(mic_raw);
        else if (audio.capturing()) mic_raw = audio.capture();
        else mic_raw.clear();
        for (s16& v : mic_raw) {
          dc += (v - dc) * 0.005;
          const double y = (v - dc) * mic_gain;
          v = static_cast<s16>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        // Capture arrives in bursts; the core wants one frame's worth every
        // frame. Queue it and hand out a frame at a time, dropping a backlog
        // beyond a few frames so a stall does not turn into latency.
        mic_queue.insert(mic_queue.end(), mic_raw.begin(), mic_raw.end());
        if (mic_queue.size() > mic_per_frame * 4) mic_queue.erase(mic_queue.begin(), mic_queue.end() - mic_per_frame * 2);
        const size_t take = mic_queue.size() < mic_per_frame ? mic_queue.size() : mic_per_frame;
        mic.assign(mic_queue.begin(), mic_queue.begin() + take);
        mic_queue.erase(mic_queue.begin(), mic_queue.begin() + take);
        if (mic_gate > 0 && !mic.empty()) {
          double sq = 0;
          for (s16 v : mic) sq += double(v) * v;
          const double rms = std::sqrt(sq / mic.size());
          mic_floor = rms < mic_floor ? rms : mic_floor + (rms - mic_floor) * 0.002;   // drops at once, creeps up
          if (rms < mic_floor * mic_gate + 16) std::fill(mic.begin(), mic.end(), 0);
        }
      }
      if (static const bool mic_log = std::getenv("DS_MIC_LOG") != nullptr; mic_log) {
        static u32 mframes = 0, mtotal = 0; static int mpeak = 0; static double msq = 0;
        for (s16 v : mic) { msq += double(v) * v; if (std::abs(int(v)) > mpeak) mpeak = std::abs(int(v)); }
        mtotal += static_cast<u32>(mic.size());
        if (++mframes == 60) {
          std::fprintf(stderr, "[mic] capture: %u samples/60 frames, peak %d, rms %.0f\n", mtotal, mpeak, mtotal ? std::sqrt(msq / mtotal) : 0.0);
          mframes = mtotal = 0; mpeak = 0; msq = 0;
        }
      }
      ds::input::decimate_mic(in, mic.data(), mic.size());
      if (log.writing()) log.write(in);
      ds::input::apply(nds, in, mic.data(), mic.size());
    }

    // With per-scanline scaling the core writes straight into the panel-sized
    // texture, so the lock has to happen before the frame runs and the scale
    // cost lands inside run_frame() rather than in the present. DS_FPS's
    // emu/draw split shifts accordingly; the total is what compares.
    const bool fast = ff_toggle || input.fast_forward_held();
    if (fast != was_fast) { was_fast = fast; next_frame = SDL_GetPerformanceCounter(); }
    const bool present = !fast || ff_skip <= 0 || frames % static_cast<u64>(ff_skip + 1) == 0;
    ds::sdl::Display::Target target[2] = {};
    bool scaled = false;
    if (present) {
      scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
    }
    for (int i = 0; i < 2; ++i)
      nds.gpu.set_scale_target(i, scaled ? ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun, grid, chunky, chunky_thresh, seam_blend, target[i].seam_w,
                                                                       static_cast<const ds::gpu::Gpu::CellMap*>((dual_window && i == bottom_display ? display2 : display).cell_map(i))}
                                         : ds::gpu::Gpu::ScaleTarget{});

    const Uint64 t0 = SDL_GetPerformanceCounter();
    nds.run_frame();

    const Uint64 t1 = SDL_GetPerformanceCounter();
    if (present) {
      const bool cursor = input.stylus_visible() && !log.reading();
      const bool slot_osd = slot_shown > 0;
      if (slot_shown > 0) --slot_shown;
      if (scaled) {
        if (cursor) draw_cursor(CursorDst{target[1].px, target[1].pitch, target[1].h, target[1].xrun}, input.stylus_x(), input.stylus_y(), input.stylus_size());
        if (slot_osd) draw_slot(CursorDst{target[0].px, target[0].pitch, target[0].h, target[0].xrun}, state_slot);
        display.end_frame();
        if (dual_window) display2.end_frame();
      } else {
        const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        if (cursor) {
          std::memcpy(cursor_fb.data(), fb[1], cursor_fb.size() * 4);
          draw_cursor(CursorDst{cursor_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, input.stylus_x(), input.stylus_y(), input.stylus_size());
          fb[1] = cursor_fb.data();
        }
        if (slot_osd) {
          std::memcpy(osd_fb.data(), fb[0], osd_fb.size() * 4);
          draw_slot(CursorDst{osd_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, state_slot);
          fb[0] = osd_fb.data();
        }
        display.draw(fb);
        if (dual_window) display2.draw(fb);
      }
    }
    audio.push(nds, fast);
    const Uint64 t2 = SDL_GetPerformanceCounter();
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;
    draw_ticks_total += t2 - t1;
    frame_ms.push_back(static_cast<double>(t1 - t0) * ticks_to_ms);
    work_ms.push_back(static_cast<double>(t2 - t0) * ticks_to_ms);

    const Uint64 t3 = SDL_GetPerformanceCounter();
    if (fast && ff_speed <= 0) {
      // unthrottled
    } else if (audio.active() && !fast) {
      audio.pace();
    } else {
      next_frame += static_cast<Uint64>(frame_ns * ticks_per_ns / (fast ? ff_speed : 1));
      const Uint64 now = SDL_GetPerformanceCounter();
      if (next_frame > now) {
        const double wait_ms = (next_frame - now) / (ticks_per_ns * 1e6);
        if (wait_ms > 1.0) SDL_Delay(static_cast<Uint32>(wait_ms));
      } else {
        next_frame = now;      // running behind: do not build up a debt
      }
    }

    pace_ticks += SDL_GetPerformanceCounter() - t3;

    ++frames;
    if (nds.cart && nds.cart->sram_dirty()) {
      if (nds.cart->sram_writes() != sram_writes_seen) { sram_writes_seen = nds.cart->sram_writes(); sram_quiet_since = frames; }
      else if (frames - sram_quiet_since >= 60) flush_save();
    }

    if (show_fps && frames % 60 == 0) {   // DS_FPS=1: speed and audio slack
      const Uint64 now = SDL_GetPerformanceCounter();
      const double secs = static_cast<double>(now - fps_mark) / SDL_GetPerformanceFrequency();
      const double to_ms = 1e3 / SDL_GetPerformanceFrequency() / 60.0;
      std::fprintf(stderr, "%.1f fps (%.0f%%), emu %.1f ms, present %.1f ms, audio queued %.1f frames\n",
                   60.0 / secs, 100.0 * (60.0 / secs) / (ds::ARM9_CLOCK_HZ / double(ds::CYCLES_PER_FRAME)),
                   emu_ticks * to_ms, draw_ticks * to_ms, audio.queued_frames());
      fps_mark = now;
      emu_ticks = draw_ticks = 0;
    }
  }

  flush_save();
  if (log.writing()) std::fprintf(stderr, "recorded %u frames to %s\n", log.frames(), record);
  // Emulation work only -- see frame_report.h. The two excluded costs are
  // named on their own line so a CLI/SDL disagreement can be attributed.
  ds::frame_report(frame_ms);
  // The same statistics over emulation + present, which is what a missed
  // display frame actually is. Only worth reading with --no-vsync: with
  // vsync on the present blocks and the tail pins to the refresh.
  ds::frame_report(work_ms, "work");
  if (!frame_ms.empty())
    std::fprintf(stderr, "  (emulation only; excluded: present %.1f ms, pacing %.1f ms total over %zu frames)\n",
                 static_cast<double>(draw_ticks_total) * ticks_to_ms,
                 static_cast<double>(pace_ticks) * ticks_to_ms, frame_ms.size());
  log.close();
  ds::prof::report();
  lid.close();
  mic_alsa.close();
  input.close();
  audio.close();
  display.close();
  display2.close();
  SDL_Quit();
  return 0;
}
