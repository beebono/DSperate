// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SDL2 frontend: direct boot, both screens stacked, sound, and input from a
// keyboard, a game controller or a touchscreen. Settings come from an INI
// file (config.h) with the command line on top; hotkeys cover what a
// handheld needs (volume, layout, screenshots, save states), and the pause
// key opens a blitted menu over the held frame (menu.h).
#include "core/nds.h"
#include "core/cart/zip.h"
#include "core/cart/zip_cache.h"
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/input/input_log.h"
#include "core/state/state.h"
#include "core/cheat/database.h"
#include "core/cart/miniz/miniz_tdef.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "lid.h"
#include "loader_cart.h"
#include "menu.h"

#include <dirent.h>
#include <algorithm>
#include "mic_alsa.h"

#include <SDL2/SDL.h>
#include <sched.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <atomic>
#include <ctime>
#include <string>
#include <vector>

namespace {

// Startup chatter and hotkey echoes: useful at a terminal, pure noise on a
// handheld where nobody reads stderr. Errors, refusals and the confirmations
// for actions that touch the disk (a state written, a screenshot taken) are
// never gated -- those are the lines you need precisely when something went
// wrong. DS_VERBOSE=1 brings the rest back.
bool verbose() { static const bool v = std::getenv("DS_VERBOSE") != nullptr; return v; }
#define VLOG(...) do { if (verbose()) std::fprintf(stderr, __VA_ARGS__); } while (0)


using namespace ds;

const char* kUsage =
    "usage: dsperate [rom.nds|rom.zip] [--bios9 F --bios7 F --firmware F] [options]\n"
    "  With no ROM (or a file named BootMenu.nds) the console boots its own\n"
    "  firmware: the DS menu, with the clock set from this machine and PictoChat.\n"
    "  --config F      settings file (default ~/.config/dsperate/dsperate.ini; every\n"
    "                  option below has a key there; games/<rom name>.ini and games/<CODE>.ini\n"
    "                  override it per game, the filename one winning)\n"
    "  --write-config F  write the default settings file (all keys commented) to F and exit\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) | horizontal | single | pip | dominant_v | dominant_h\n"
    "  --screen S      top (default) or bottom: the screen shown alone, large or dominant\n"
    "  --pip-alpha X   opacity of the PiP inset at rest, 0..1 (default 1; it comes up to opaque\n"
    "                  while the bottom screen is touched)\n"
    "  --dual-window   one window per video display, one DS screen each (dual-panel\n"
    "                  handhelds; also what direct scanout needs on them)\n"
    "  --linear        bilinear scaling instead of nearest (takes precedence over the grid, seams\n"
    "                  and chunky)\n"
    "  --lcd-grid S    LCD pixel grid strength, 0 (off, default) .. 1 (software scaling, or the\n"
    "                  display engine's overlay layer on the A30)\n"
    "  --seam S        dark (default): the LCD grid, dimmed by --lcd-grid | blend: box-filter seams\n"
    "                  (sharp-shimmerless): the one panel pixel/row that straddles two DS pixels is their\n"
    "                  area-weighted blend, all others crisp | blend_linear: the same in linear light\n"
    "  --chunky [M]    draw each 2x2 block of DS pixels as one cell (with the grid, one seam per\n"
    "                  block); lower resolution, for panels at fractional scales. M: mean (default;\n"
    "                  the area-weighted box) | extreme (the mean, unless the darkest or brightest\n"
    "                  pixel stands more than --chunky-threshold N (0..255, default 180) from it) |\n"
    "                  mode (dominant colour, mean when all differ) | tl (top-left pixel) | min | max\n"
    "  --chunky-cell C panel pixels per chunky cell: auto (default; the smallest of 4..16 that divides the\n"
    "                  screen, 4 on a 640x480 panel = 160x120 cells) | pair (2x2 DS pixels) | N (the\n"
    "                  nearest size at or below N that divides the screen)\n"
    "  --disp / --no-disp  present through the display engine's scaler layer (Miyoo A30 class\n"
    "                  devices; the default is auto: wherever /dev/disp answers). video.disp\n"
    "  --fbdev / --no-fbdev  present straight through /dev/fb0 (the mali-fbdev SDL2 of the\n"
    "                  H700 handhelds; the default is auto: when that SDL2 has a mali driver\n"
    "                  and fb0 answers). video.fbdev\n"
    "  --no-audio      run without sound (frames are paced by the clock)\n"
    "  --volume N      0..100\n"
    "  --no-mic        do not open the microphone (M still fakes one)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --timing-oc     Timing OC: no GX FIFO, untimed geometry (faster, less accurate; DraStic's model).\n"
    "                  emu.timing_oc in the config\n"
    "  --cpu-oc        CPU OC: recompiled data accesses priced as main RAM (less accurate); emu.cpu_oc\n"
    "  --fast-load     cart DMA reads the card without its clock (may affect accuracy); emu.fast_load\n"
    "  --aa / --no-aa  3D anti-aliasing on (hardware behaviour) or off; video.aa, off by default\n"
    "  --lockstep      128-cycle CPU interleave (melonDS lockstep) instead of event-bound; --quantum N for any value\n"
    "  --frameskip N   skip drawing up to N frames in N+1 (0 = off); emu.frameskip. Skipping runs\n"
    "                  in whole display periods, so on a game that drives its screens on\n"
    "                  alternate frames the limit counts pairs (DS_DEBUG_SKIP=1 shows the period)\n"
    "  --frameskip-mode M  adaptive (default; skip only while the emulator is behind, up to N)\n"
    "                  | fixed (always skip N of every N+1)\n"
    "  --frameskip-capture  skip frames that display-capture too (INEXACT: the captured VRAM\n"
    "                  holds the last drawn frame). Without it a game that captures every\n"
    "                  frame -- Pokemon B/W, Golden Sun -- skips nothing; emu.frameskip_capture\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --stats-from N  leave the first N frames out of the frame statistics (DS_FRAME_STATS)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n"
    "  --rtc-host      run the clock from this machine even under --replay (INEXACT: a game\n"
    "                  that reads the date no longer replays the same, but the firmware's own\n"
    "                  menu needs a real clock to appear at all)\n"
    "  --load-state F  start from a save state instead of booting the game\n"
    "  --autosave-png F  with emu.autosave, write a PNG of both screens to F beside the auto state\n"
    "  --save F        battery save to start from, instead of <rom>.sav\n"
    "                  (a --replay never writes the save back, so a scene repeats)\n"
    "  --clear-cache   delete every unpacked zipped game (the .dsperate directories beside the\n"
    "                  games and paths.cache) except the one being launched, then run as usual\n";

std::string rom_dir_of(const std::string& rom) {
  const size_t slash = rom.find_last_of('/');
  return slash == std::string::npos ? "." : rom.substr(0, slash);
}

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
// The extension, lower-cased ("" if none): the command line's test for a game.
std::string rom_ext(const std::string& rom) {
  const std::string stem = rom_stem(rom);
  std::string ext = rom.substr(stem.size());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
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
  VLOG("save: loaded %zu bytes from %s\n", n, path.c_str());
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

// Both screens into one PNG laid out as the window shows them -- the same
// placement the display uses, at the layout's natural size (one DS pixel per
// pixel for the full-size screens; an inset or the smaller of a dominant
// pair is sampled down). Whoever shows it scales as it likes. Read from the
// emulator's framebuffers, not the panel: a screen grabber cannot see a
// hardware scaler layer, and the panel buffers carry the grid, chunky and
// seams, which a screenshot of the game does not want. On a scanline tier
// those framebuffers are only filled by an unscaled frame, which is why the
// hotkey defers a frame (shot_pending). Straight RGBA, alpha forced opaque.
bool write_png(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& layout) {
  using Disp = ds::sdl::Display;
  int w = 0, h = 0;
  Disp::natural_size(layout, 1.0, w, h);
  Disp::View views[Disp::SCREENS];
  Disp::place(layout, w, h, views);
  std::vector<u8> rgba(static_cast<size_t>(w) * h * 4, 0);
  for (int i = 0; i < Disp::SCREENS; ++i) {
    const Disp::View& v = views[i];
    if (!v.shown || v.rect.w <= 0 || v.rect.h <= 0) continue;
    const u32* fb = nds.gpu.framebuffer(v.screen);
    // The inset at its resting opacity (the touch ramp is a live-only thing).
    const u32 alpha = v.direct ? 255 : static_cast<u32>(std::clamp(layout.pip_alpha, 0.0, 1.0) * 255.0 + 0.5);
    for (int y = 0; y < v.rect.h; ++y) {
      const int dy = v.rect.y + y;
      if (dy < 0 || dy >= h) continue;
      const int sy = static_cast<int>(static_cast<long>(y) * ds::SCREEN_H / v.rect.h);
      u8* dst = rgba.data() + (static_cast<size_t>(dy) * w + v.rect.x) * 4;
      for (int x = 0; x < v.rect.w; ++x) {
        const int dx = v.rect.x + x;
        if (dx < 0 || dx >= w) continue;
        const int sx = static_cast<int>(static_cast<long>(x) * ds::SCREEN_W / v.rect.w);
        u32 c = fb[sy * ds::SCREEN_W + sx];    // ARGB8888
        if (alpha < 255) {
          u32 under = (static_cast<u32>(dst[x * 4 + 0]) << 16) | (static_cast<u32>(dst[x * 4 + 1]) << 8) | dst[x * 4 + 2];
          Disp::blend_row(&under, &c, 1, alpha);
          c = under;
        }
        dst[x * 4 + 0] = (c >> 16) & 0xFF;
        dst[x * 4 + 1] = (c >> 8) & 0xFF;
        dst[x * 4 + 2] = c & 0xFF;
        dst[x * 4 + 3] = 0xFF;
      }
    }
  }
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6 /* zlib default level */, MZ_FALSE);
  if (!png) { std::fprintf(stderr, "png: cannot encode\n"); return false; }
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  const bool ok = f && std::fwrite(png, 1, len, f) == len;
  if (f) std::fclose(f);
  std::free(png);   // miniz allocates with plain malloc (MZ_MALLOC is not overridden)
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "png: cannot write %s\n", path.c_str()); std::remove(tmp.c_str()); return false; }
  return true;
}

// The screenshot hotkey: <GAMECODE>-<timestamp>.png in the screenshots
// directory (paths.screenshots; the states directory unless set). The
// directory is made on demand, one level, so a fresh path in the config
// works without a prior mkdir.
void screenshot(NDS& nds, const std::string& dir, const ds::sdl::Display::Layout& layout) {
  ::mkdir(dir.c_str(), 0755);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  const std::string path = dir + "/" + code + "-" + stamp + ".png";
  if (write_png(nds, path, layout)) std::fprintf(stderr, "screenshot: %s\n", path.c_str());
}

std::string state_path(NDS& nds, const std::string& dir, int slot) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + "." + std::to_string(slot) + ".dss";
}

// The auto-save slot. Named ".auto" rather than a number so it can never
// collide with a slot the player picks, and so refresh_slots() -- which walks
// 0..9 -- leaves it out of the menu: it is reached only by --load-state.
std::string auto_state_path(NDS& nds, const std::string& dir) {
  const std::string code(nds.cart ? nds.cart->header().game_code : "NONE", 4);
  return dir + "/" + code + ".auto.dss";
}

// The screen layout rides along after the machine's chunks, so a state
// brings its view back with it -- the pause menu load, the hotkey, and the
// launcher's --load-state of the auto slot all restore it. The chunk is the
// frontend's, not the core's: the headless build never writes it, and an
// older file simply ends where it would begin, so neither needs a format
// version bump. Loading it is refused during --replay along with the
// rest of the state; pip and dominant sizes are clamped as from the config.
void write_layout_chunk(ds::state::Writer& w, const ds::sdl::Display::Layout& l) {
  w.begin("VIEW");
  w.put(static_cast<u8>(l.mode)); w.put(static_cast<u8>(l.primary)); w.put(static_cast<u8>(l.corner));
  w.put(l.pip); w.put(l.dominant);
  w.end();
}

bool read_layout_chunk(ds::state::Reader& r, ds::sdl::Display::Layout& l) {
  using Disp = ds::sdl::Display;
  if (r.at_end() || !r.begin("VIEW")) return false;
  u8 mode = 0, primary = 0, corner = 0; double pip = l.pip, dominant = l.dominant;
  r.fields(mode, primary, corner);
  if (r.more()) r.fields(pip, dominant);
  r.end();
  if (!r.ok() || mode >= static_cast<u8>(Disp::Mode::Count) || primary > 1 || corner >= static_cast<u8>(Disp::Corner::Count)) return false;
  l.mode = static_cast<Disp::Mode>(mode); l.primary = primary; l.corner = static_cast<Disp::Corner>(corner);
  l.pip = std::clamp(pip, 0.1, 0.9); l.dominant = std::clamp(dominant, 0.1, 0.99);
  return true;
}

bool save_state_file(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& layout) {
  ds::state::Writer w; std::string err;
  if (!nds.save_state(w, err)) { std::fprintf(stderr, "state: cannot save: %s\n", err.c_str()); return false; }
  write_layout_chunk(w, layout);
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
// error says so). `layout` is set to the view the state carries, when it
// carries one (a headless or older file does not), and left alone otherwise.
bool load_state_file(NDS& nds, const std::string& path, ds::sdl::Display::Layout& layout, bool& layout_loaded) {
  layout_loaded = false;
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
  layout_loaded = read_layout_chunk(r, layout);
  std::fprintf(stderr, "state: loaded %s (frame %llu%s)\n", path.c_str(), static_cast<unsigned long long>(nds.frame_count),
               layout_loaded ? (std::string(", layout ") + ds::sdl::Display::mode_name(layout.mode)).c_str() : "");
  return true;
}

// The game library, for the picker the loader cart raises.
//
// The row is the filename without its extension, not the ROM header's own
// 12-byte title. The header title was the plan, on the theory that a filename
// is cryptic; a look at three real ROMs says otherwise -- "ARTACADEMYRT",
// "FF3" and "LEGENDOFKAY" against "Art Academy", "Final Fantasy III" and
// "Legend of Kay". The filename is what the player named the file, and it is
// the one thing about a library they control.
std::vector<ds::sdl::Menu::GameEntry> enumerate_games(const std::string& dir) {
  std::vector<ds::sdl::Menu::GameEntry> games;
  if (dir.empty()) return games;
  DIR* d = opendir(dir.c_str());
  if (!d) { std::fprintf(stderr, "games: cannot read %s\n", dir.c_str()); return games; }
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) continue;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "nds" && ext != "zip") continue;
    games.push_back({rom_stem(name), dir + "/" + name});
  }
  closedir(d);
  // By what the list shows, so the order on screen is the order it is read in.
  std::sort(games.begin(), games.end(), [](const ds::sdl::Menu::GameEntry& a, const ds::sdl::Menu::GameEntry& b) {
    return a.title < b.title;
  });
  return games;
}


// Everything keyed to which ROM is in the slot: where its battery save, its
// states and screenshots and its cheats live, and which per-game .ini a
// hotkey writes to. Boot fills this once; launching a game from the loader
// cart resets the machine and opens it again, because otherwise a session
// begun on BootMenu.nds would go on writing the loader's paths and its own
// game code for the game the player actually chose.
//
// Config layering is deliberately *not* redone on a re-open: the per-game
// .ini is merged into cfg at boot, and merging a second game's over the top
// would accumulate rather than replace. Only game_ini itself is re-keyed, so
// a layout picked with a hotkey after a launch lands on the right game.
struct Session {
  std::string rom_path;         // what is in the slot
  std::string rom_dir;          // its directory: the default home for everything below
  std::string game_ini;         // per-game settings; empty when there is no cart
  std::string states_dir;       // save states (and the autosave's PNG)
  std::string shots_dir;        // manual screenshots (F9); paths.screenshots, else states_dir
  std::string sav;              // battery save
  std::string cheats_on_path;   // which cheats are on, one name per line
  ds::cheat::GameCheats cheats; // the database entry for this ROM; the menu points at its groups

  // `save_arg` is --save, which pins the battery save whatever the ROM is.
  void open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg);
  void load_enabled(NDS& nds) const;
  void save_enabled(NDS& nds) const;
};

void Session::open(NDS& nds, const ds::sdl::Config& cfg, const std::string& rom, const char* save_arg) {
  rom_path = rom;
  const size_t slash = rom_path.find_last_of('/');
  rom_dir = slash == std::string::npos ? "." : rom_path.substr(0, slash);
  states_dir = cfg.str("paths.states", rom_dir);
  shots_dir = cfg.str("paths.screenshots", states_dir);
  sav = save_arg ? std::string(save_arg) : save_path(rom_path, cfg.str("paths.saves"));
  game_ini.clear();
  if (nds.cart) {
    game_ini = ds::sdl::Config::game_path_rom(rom_path);
    if (game_ini.empty()) game_ini = ds::sdl::Config::game_path_code(nds.cart->header().game_code);
  }

  // Cheats: a usrcheat.dat, from [paths] cheats or beside the ROM or in the
  // config directory. The entry matching this ROM's game code and header
  // checksum is loaded; nothing is enabled by that alone, so a database that
  // is simply present costs a file read at startup and nothing after it.
  cheats = ds::cheat::GameCheats{};
  nds.cheats.codes.clear();
  cheats_on_path.clear();
  std::string db = cfg.str("paths.cheats");
  // The configured path may name the file itself or the directory holding it;
  // a directory that already ends in usrcheat.dat must not get a second one.
  if (!db.empty()) {
    auto ends_with_db = [](const std::string& s) {
      static const std::string tail = "usrcheat.dat";
      return s.size() >= tail.size() &&
             s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    };
    while (db.size() > 1 && (db.back() == '/' || db.back() == '\\')) db.pop_back();
    if (!ends_with_db(db)) {
      std::string joined = db + "/usrcheat.dat";
      if (FILE* f = std::fopen(joined.c_str(), "rb")) { std::fclose(f); db = joined; }
    }
  }
  if (db.empty()) {
    for (const std::string& candidate : {rom_dir + "/usrcheat.dat", ds::sdl::Config::dir() + "/usrcheat.dat"}) {
      if (FILE* f = std::fopen(candidate.c_str(), "rb")) { std::fclose(f); db = candidate; break; }
    }
  }
  if (!db.empty()) {
    std::string err;
    if (ds::cheat::load_for_rom(db, rom_path, cheats, err)) {
      VLOG("cheats: %s -- %zu codes in %zu groups\n", cheats.name.c_str(), cheats.codes.size(), cheats.groups.size());
      nds.cheats.codes = cheats.codes;
    } else if (!err.empty()) {
      std::fprintf(stderr, "cheats: %s\n", err.c_str());
    }
  }
  // Which cheats are on is remembered per game, next to the save states, as
  // one code name per line. Names rather than indices: a database update
  // renumbers everything, and a name that no longer exists is simply dropped.
  if (!cheats.codes.empty())
    cheats_on_path = states_dir + "/" + std::string(nds.cart ? nds.cart->header().game_code : "NONE", 4) + ".cheats";
}

void Session::load_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "rb");
  if (!f) return;
  char line[512];
  size_t on = 0;
  while (std::fgets(line, sizeof line, f)) {
    std::string want(line);
    while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) want.pop_back();
    if (want.empty()) continue;
    for (ds::cheat::Code& c : nds.cheats.codes)
      if (!c.is_note() && c.name == want) { c.enabled = true; ++on; }
  }
  std::fclose(f);
  if (on) VLOG("cheats: %zu enabled from %s\n", on, cheats_on_path.c_str());
}

void Session::save_enabled(NDS& nds) const {
  if (cheats_on_path.empty()) return;
  FILE* f = std::fopen(cheats_on_path.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "cheats: cannot write %s\n", cheats_on_path.c_str()); return; }
  for (const ds::cheat::Code& c : nds.cheats.codes)
    if (c.enabled && !c.is_note()) std::fprintf(f, "%s\n", c.name.c_str());
  std::fclose(f);
}

// The stick-driven pen: an outlined crosshair with a red centre, drawn over
// the bottom screen in DS pixel space and mapped onto the destination (the
// frontend's scaled buffer, or a copy of the framebuffer).
using CursorDst = ds::sdl::Blit;   // px/pitch/h and the DS-column map; xrun null: 1:1
void draw_cursor(const CursorDst& d, int cx, int cy, int size) {
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x > 255 || y < 0 || y > 191) return;
    const ds::sdl::BlitRect r = ds::sdl::blit_rect(d, x, y);
    for (u32 yy = r.y0; yy < r.y1; ++yy) for (u32 xx = r.x0; xx < r.x1; ++xx) d.px[yy * d.pitch + xx] = colour;
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

// A small white number (3x5 font, doubled) on a black box, in DS screen
// coordinates so it lands the same whether the frame was scaled straight into
// the window (the xrun path) or copied first. `right` anchors the box to the
// right edge instead of the left, which is how the two callers -- the state
// slot in the top-left, the FPS counter in the top-right -- stay clear of
// each other. A value too wide for the field saturates to all nines.
// `bottom` anchors to the bottom edge instead of the top, which is how an
// overlay steps out of the way of the PiP inset sharing its corner.
void draw_number(const CursorDst& d, int value, int digits, bool right, bool bottom) {
  static const u8 font[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}};
  auto fill = [&](int x, int y, u32 colour) {
    const ds::sdl::BlitRect r = ds::sdl::blit_rect(d, x, y);
    for (u32 yy = r.y0; yy < r.y1; ++yy) for (u32 xx = r.x0; xx < r.x1; ++xx) d.px[yy * d.pitch + xx] = colour;
  };
  if (value < 0) value = 0;
  if (digits < 1) digits = 1;
  if (digits > 8) digits = 8;
  // Saturate rather than let the loop below keep the low digits: 1234 in a
  // 3-digit field reads as 999, never as 234.
  int cap = 1; for (int i = 0; i < digits; ++i) cap *= 10;
  if (value >= cap) value = cap - 1;
  int glyph[8];                                      // most significant first
  int n = 0;
  do { glyph[n++] = value % 10; value /= 10; } while (value != 0 && n < digits);
  for (int i = 0; i < n / 2; ++i) { const int t = glyph[i]; glyph[i] = glyph[n - 1 - i]; glyph[n - 1 - i] = t; }
  const int S = 2;                                   // glyph scale
  const int w = n * 3 * S + (n - 1) * S + 4;         // glyphs, one S-wide gap between each, 2px border
  const int h = 5 * S + 4;
  const int X = right ? static_cast<int>(ds::SCREEN_W) - 4 - w : 4;
  const int Y = bottom ? static_cast<int>(ds::SCREEN_H) - 4 - h : 4;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) fill(X + x, Y + y, 0xFF000000);
  for (int g = 0; g < n; ++g)
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
      if ((font[glyph[g]][r] >> (2 - c)) & 1)
        for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x)
          fill(X + 2 + g * 4 * S + c * S + x, Y + 2 + r * S + y, 0xFFFFFFFF);
}

// The state slot, shown briefly after a slot hotkey.
void draw_slot(const CursorDst& d, int digit, bool bottom) { draw_number(d, digit, 1, false, bottom); }

// The screenshot flash: white blended over a whole screen at `alpha`, the
// frame after the picture is taken and fading over FLASH_FRAMES. It is drawn
// where the other overlays are -- into the presented buffer, never into the
// GPU's framebuffers the screenshot reads -- so it is never in the picture.
constexpr int FLASH_FRAMES = 12;
void draw_flash(const CursorDst& d, u32 alpha) {
  const ds::sdl::BlitRect r0 = ds::sdl::blit_rect(d, 0, 0), r1 = ds::sdl::blit_rect(d, ds::SCREEN_W - 1, ds::SCREEN_H - 1);
  const u32 w = r1.x1 - r0.x0;
  if (!w || r1.y1 <= r0.y0) return;
  std::vector<u32> white(w, 0xFFFFFFFFu);
  for (u32 y = r0.y0; y < r1.y1; ++y) ds::sdl::Display::blend_row(d.px + y * d.pitch + r0.x0, white.data(), w, alpha);
}

// A launcher's SIGTERM (or Ctrl-C) must still flush the battery save.
volatile std::sig_atomic_t g_signalled = 0;
void on_signal(int) { g_signalled = 1; }

} // namespace

int main(int argc, char** argv) {
  const char* rom = nullptr;
  const char* config_arg = nullptr;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr, *save_arg = nullptr, *load_state = nullptr;
  bool clear_cache = false;
  bool rtc_host = false;              // --rtc-host: a real clock even under a replay (the firmware menu needs one)
  long stats_from = 0;   // frames run but left out of the timing statistics

  // The game is found before the options are read. A flag whose value is
  // optional (--chunky [M]) takes the next word unless it is another option,
  // so `--chunky game.nds` used to swallow the game as its mode and boot the
  // firmware instead. A word that looks like a game -- a .nds or .zip, or a
  // path to a file that exists -- is the ROM wherever it sits, and is never
  // read as a value by such a flag.
  const char* rom_word = nullptr;
  auto looks_like_rom = [](const char* w) {
    if (w[0] == '-') return false;
    const std::string ext = rom_ext(w);
    if (ext == ".nds" || ext == ".zip") return true;
    struct stat st;
    return ::stat(w, &st) == 0 && S_ISREG(st.st_mode);
  };
  for (int i = 1; i < argc && !rom_word; ++i) if (looks_like_rom(argv[i])) rom_word = argv[i];
  // The command line is one more settings layer, applied after the files.
  ds::sdl::Config cli;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    // The value of a flag that need not have one: the next word, unless it
    // is an option or the game.
    auto optional = [&](const char* fallback) { return i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1] != rom_word ? argv[++i] : fallback; };
    if (arg("--bios9")) cli.set("paths.bios9", argv[++i]);
    else if (arg("--bios7")) cli.set("paths.bios7", argv[++i]);
    else if (arg("--firmware")) cli.set("paths.firmware", argv[++i]);
    else if (arg("--config")) config_arg = argv[++i];
    else if (arg("--write-config")) { ds::sdl::Config::write_default(argv[++i], true); return 0; }
    else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--screen")) cli.set("video.screen", argv[++i]);
    else if (arg("--pip-alpha")) cli.set("video.pip_alpha", argv[++i]);
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (flag("--rtc-host")) rtc_host = true;
    else if (flag("--clear-cache")) clear_cache = true;
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (arg("--load-state")) load_state = argv[++i];
    else if (arg("--autosave-png")) cli.set("emu.autosave_png", argv[++i]);
    // A state load starts cold -- every translated block went with the old
    // run and the caches hold the loader's data -- so the first frames are
    // slow in a way the scene never is. Measured on the RG DS: ~13 ms on max
    // and ~2 ms on p99 over the first 15 frames, with the mean unmoved.
    else if (arg("--stats-from")) stats_from = std::atol(argv[++i]);
    else if (flag("--fullscreen")) cli.set("video.fullscreen", "true");
    else if (flag("--linear")) cli.set("video.linear", "true");
    else if (arg("--lcd-grid")) cli.set("video.lcd_grid", argv[++i]);
    else if (flag("--chunky")) cli.set("video.chunky", optional("mean"));
    else if (arg("--chunky-threshold")) cli.set("video.chunky_threshold", argv[++i]);
    else if (arg("--chunky-cell")) cli.set("video.chunky_cell", argv[++i]);
    else if (arg("--seam")) cli.set("video.seam", argv[++i]);
    else if (flag("--disp")) cli.set("video.disp", "true");
    else if (flag("--no-disp")) cli.set("video.disp", "false");
    else if (flag("--fbdev")) cli.set("video.fbdev", "true");
    else if (flag("--no-fbdev")) cli.set("video.fbdev", "false");
    else if (flag("--no-audio")) cli.set("audio.enabled", "false");
    else if (arg("--volume")) cli.set("audio.volume", argv[++i]);
    else if (flag("--no-mic")) cli.set("audio.mic", "false");
    else if (flag("--no-vsync")) cli.set("video.vsync", "false");
    else if (flag("--interp")) cli.set("emu.jit", "false");
    else if (flag("--lockstep")) cli.set("emu.quantum", std::to_string(ds::LOCKSTEP_QUANTUM));
    else if (arg("--quantum")) cli.set("emu.quantum", argv[++i]);
    else if (flag("--timing-oc")) cli.set("emu.timing_oc", "true");
    else if (flag("--cpu-oc")) cli.set("emu.cpu_oc", "true");
    else if (flag("--fast-load")) cli.set("emu.fast_load", "true");
    else if (arg("--frameskip")) cli.set("emu.frameskip", argv[++i]);
    else if (arg("--frameskip-mode")) cli.set("emu.frameskip_mode", argv[++i]);
    else if (flag("--frameskip-capture")) cli.set("emu.frameskip_capture", "true");
    else if (flag("--aa")) cli.set("video.aa", "true");
    else if (flag("--no-aa")) cli.set("video.aa", "false");
    // The two halves of Timing OC separately: they pull in opposite directions
    // on Golden Sun, so the bundled flag reads flat while neither half is.
    else if (flag("--help")) { std::fputs(kUsage, stderr); return 0; }
    else if (argv[i][0] == '-' && argv[i][1] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); std::fputs(kUsage, stderr); return 2; }
    else rom = argv[i];
  }
  ds::sdl::Config cfg;
  const std::string global_ini = config_arg ? std::string(config_arg) : ds::sdl::Config::global_path();
  if (!config_arg) ds::sdl::Config::write_default(global_ini);
  if (!cfg.load(global_ini) && config_arg) { std::fprintf(stderr, "cannot read %s\n", config_arg); return 2; }
  auto apply_cli = [&] { for (const char* k : {"paths.bios9", "paths.bios7", "paths.firmware", "video.scale", "video.dual_window", "video.layout", "video.screen", "video.pip_alpha",
                                              "video.fullscreen", "video.linear", "video.lcd_grid", "video.chunky", "video.chunky_threshold", "video.chunky_cell", "video.seam", "video.disp", "video.fbdev", "video.vsync", "audio.enabled", "audio.volume",
                                              "audio.mic", "emu.jit", "emu.quantum", "emu.timing_oc", "emu.cpu_oc", "emu.fast_load", "emu.frameskip", "emu.frameskip_mode", "emu.frameskip_capture", "video.aa", "emu.autosave_png"}) if (cli.has(k)) cfg.set(k, cli.str(k)); };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7"), fw = cfg.str("paths.firmware");
  if (bios9.empty() || bios7.empty() || fw.empty()) { std::fprintf(stderr, "BIOS and firmware paths are needed (--bios9/--bios7/--firmware or [paths] in %s)\n", global_ini.c_str()); return 2; }

  // No ROM boots the firmware's own menu. A file called BootMenu.nds selects
  // the same thing without a command line -- a launcher that only knows how to
  // start games can point at one, and it never needs to exist. Either way a
  // path is settled on here rather than threaded through as "no ROM": every
  // per-game path below (config, saves, states, screenshots, cheats) is
  // derived from this string, and they all want somewhere to live.
  //
  // If BootMenu.nds *does* exist it is put in the slot as well, still under a
  // firmware boot. That is how the loader cart (tools/mkcart.py) is meant to
  // be paired with a real firmware dump: the DS menu draws its banner, and
  // tapping it plays the console's own launch animation.
  const bool boot_firmware = !rom || rom_stem(base_name(rom)) == "BootMenu";
  const std::string rom_path = rom ? std::string(rom) : ds::sdl::Config::dir() + "/BootMenu.nds";
  if (boot_firmware) VLOG("no game: booting the firmware\n");

  // Real-time scheduling for the whole process: set here, before any thread
  // exists (the NDS below starts the compositor), so the emulation thread,
  // the compositor, the band workers and SDL's own threads all inherit it. On the RG DS (four cores that the
  // emulator fills, a compositor and a sound daemon on the same cores at
  // normal priority) Golden Sun's p90 went from 16.8-17.4 ms to 14.7-15.0
  // and p99 from 23 to 18 with audio on (2026-09-04): the tail was
  // preemption, not work. Needs the privilege (root, or an rtprio limit);
  // refused quietly otherwise. emu.realtime = off | rr | fifo, emu.rt_priority.
  {
    const std::string rt = cfg.str("emu.realtime", "rr");
    const int prio = cfg.num("emu.rt_priority", 5);
    if (rt == "rr" || rt == "fifo") {
      sched_param sp{}; sp.sched_priority = prio;
      if (sched_setscheduler(0, rt == "rr" ? SCHED_RR : SCHED_FIFO, &sp) != 0)
        VLOG("realtime scheduling (%s %d) not permitted: %s\n", rt.c_str(), prio, std::strerror(errno));
      else VLOG("realtime scheduling: %s %d\n", rt.c_str(), prio);
    }
  }
  NDS nds;
  if (!nds.load_bios(bios9.c_str(), bios7.c_str(), fw.c_str())) { std::fprintf(stderr, "could not load BIOS/firmware\n"); return 1; }
  nds.reset();
  // The firmware writes its settings pages to flash over SPI. Those go to a
  // sidecar beside the firmware rather than into the dump itself, so a rename
  // in the DS menu survives a restart without the emulator ever writing to a
  // file the user cannot regenerate. See NDS::load_firmware_override.
  const std::string fw_override = cfg.str("paths.firmware_override", fw + ".ovr");
  {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) {
      if (err != "cannot open") std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    } else if (err.empty()) {
      VLOG("firmware settings: %s\n", fw_override.c_str());   // loaded cleanly: chatter
    } else {
      std::fprintf(stderr, "firmware settings: %s -- warning: %s\n", fw_override.c_str(), err.c_str());
    }
  }
  // Core knobs that the core reads from the environment.
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP")) setenv("DS_IDLE_SKIP", cfg.str("emu.idle_skip").c_str(), 1);

  int scale = cfg.num("video.scale", 2);
  if (scale < 1) scale = 1;
  const bool fullscreen = cfg.flag("video.fullscreen", false), linear = cfg.flag("video.linear", false);
  // Grid strength -> brightness kept on the seams, 0..256 (256 = off).
  const double grid_s = std::min(1.0, std::max(0.0, cfg.real("video.lcd_grid", 0.0)));
  const u32 grid = static_cast<u32>(std::lround((1.0 - grid_s) * 256.0));
  u8 seam_blend = 0;
  {
    const std::string sm = cfg.str("video.seam", "dark");
    if (sm == "blend") seam_blend = 1; else if (sm == "blend_linear") seam_blend = 2;
    else if (sm != "dark") { std::fprintf(stderr, "unknown seam %s (dark | blend | blend_linear)\n", sm.c_str()); return 2; }
    if (linear && (grid_s > 0.0 || seam_blend || cfg.flag("video.chunky", false) || cfg.str("video.chunky", "false") != "false"))
      std::fprintf(stderr, "video.linear takes precedence over lcd_grid, seam and chunky\n");
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
  using Menu = ds::sdl::Menu;
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
    layout.pip_alpha = std::clamp(cfg.real("video.pip_alpha", 1.0), 0.0, 1.0);
    layout.dominant = std::clamp(cfg.real("video.dominant_ratio", 0.5), 0.1, 0.99);
  }
  ds::prof::enabled = std::getenv("DS_PROFILE") != nullptr;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // The display-engine tier (display_disp.h) owns the panel itself, so SDL
  // must not put an EGL window on it: its dummy video driver keeps the window,
  // events and controllers and draws nothing. Decided before SDL_Init, which is
  // where the driver is chosen. auto (the default) takes it wherever the
  // device answers the disp ioctls; on/off force it.
  // Whichever headless driver this SDL2 was built with: the handheld
  // builds drop "dummy" but keep "offscreen".
  auto go_headless = [](const char* tier) {
    if (std::getenv("SDL_VIDEODRIVER")) return;
    const char* pick = nullptr;
    for (const char* want : {"dummy", "offscreen"}) {
      for (int i = 0; i < SDL_GetNumVideoDrivers() && !pick; ++i) if (!std::strcmp(SDL_GetVideoDriver(i), want)) pick = want;
      if (pick) break;
    }
    if (pick) {
      setenv("SDL_VIDEODRIVER", pick, 1);
      // A headless driver's window never takes keyboard focus, and SDL
      // drops every joystick event while a window exists without focus
      // (SDL_PrivateJoystickShouldIgnoreEvent). The pad is the only
      // input on these devices, so let it through regardless.
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    } else std::fprintf(stderr, "%s: this SDL2 has no headless video driver; its own driver will also open the panel\n", tier);
  };
  const std::string disp_mode = cfg.str("video.disp");
  const bool disp_auto = disp_mode.empty() || disp_mode == "auto";
  bool use_disp = disp_mode == "true" || disp_mode == "on";
  if ((use_disp || disp_auto) && !dual_window) {
    if (ds::sdl::DispOut::available()) { use_disp = true; go_headless("video.disp"); }
    else if (use_disp) {
      std::fprintf(stderr, "video.disp: /dev/disp not usable; using SDL\n");
      use_disp = false;
    }
  } else use_disp = false;
  if (use_disp && chunky != 0 && chunky != 2) {
    std::fprintf(stderr, "chunky %s: the display-engine tier draws chunky cells in the scaler as their mean; using mean\n", cfg.str("video.chunky").c_str());
    chunky = 2;
  }

  // The fbdev tier (display_fbdev.h) owns fb0 the same way. auto takes it
  // only where SDL2 was built with the mali video driver -- the BaseOS
  // handhelds -- since any desktop with a console has an fb0 too and its
  // compositor, not us, should have it. on forces it wherever fb0 answers.
  const std::string fbdev_mode = cfg.str("video.fbdev");
  const bool fbdev_auto = fbdev_mode.empty() || fbdev_mode == "auto";
  bool use_fbdev = fbdev_mode == "true" || fbdev_mode == "on";
  if (!use_disp && (use_fbdev || fbdev_auto) && !dual_window) {
    // The signals: the launcher named the mali driver, this SDL2 was built
    // with one, the launcher named a headless driver outright, or nothing
    // SDL could draw on exists -- no X or Wayland display, no DRM node --
    // in which case SDL lands on its offscreen driver by itself (seen on
    // the RG35XX SP through spruce's launcher: "software renderer,
    // offscreen driver", a black panel with sound). A box like that with a
    // writable fb0 is an fbdev box.
    bool mali = false, headless = false;
    if (const char* vd = std::getenv("SDL_VIDEODRIVER")) {
      mali = !std::strcmp(vd, "mali");
      headless = !std::strcmp(vd, "dummy") || !std::strcmp(vd, "offscreen");
    } else {
      for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) if (!std::strcmp(SDL_GetVideoDriver(i), "mali")) mali = true;
      headless = !std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") && ::access("/dev/dri", F_OK) != 0;
    }
    if ((use_fbdev || mali || headless) && ds::sdl::FbdevOut::available()) {
      use_fbdev = true;
      // The mali driver named explicitly by the launcher would put an EGL
      // surface on fb0 under us; a headless one named is kept as it is.
      if (mali && std::getenv("SDL_VIDEODRIVER")) unsetenv("SDL_VIDEODRIVER");
      go_headless("video.fbdev");
    } else if (use_fbdev) {
      std::fprintf(stderr, "video.fbdev: /dev/fb0 not usable; using SDL\n");
      use_fbdev = false;
    } else use_fbdev = false;
  } else use_fbdev = false;

  u32 init = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (audio_on || mic_on) init |= SDL_INIT_AUDIO;
  // The audio backend. SDL's default on a PipeWire system is its Pulse
  // client, which on the RG DS costs Golden Sun ~1.5 ms a frame of work in
  // the daemon and its wakeups; SDL's native pipewire backend costs a third
  // of that (2026-09-04). Asked for first when the user set nothing, with
  // SDL's own choice as the fallback if it is not built in.
  const std::string audio_driver = cfg.str("audio.driver", "pipewire");
  const bool driver_forced = std::getenv("SDL_AUDIODRIVER") != nullptr;
  if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty()) setenv("SDL_AUDIODRIVER", audio_driver.c_str(), 1);
  if (SDL_Init(init) != 0) {
    if ((init & SDL_INIT_AUDIO) && !driver_forced && !audio_driver.empty()) {
      unsetenv("SDL_AUDIODRIVER");
      if (SDL_Init(init) == 0) goto sdl_ready;
    }
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    if (!(init & SDL_INIT_AUDIO) || SDL_Init(init & ~SDL_INIT_AUDIO) != 0) return 1;
    audio_on = mic_on = false;   // no audio subsystem: run silent
  }
sdl_ready:

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
    if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout, 0, 1 - bottom_display) ||
        !display2.open("DSperate (Bottom)", scale, fullscreen, linear, vsync, layout, 1, bottom_display)) { SDL_Quit(); return 1; }
    if (display.scaling() != display2.scaling()) { std::fprintf(stderr, "dual-window: mixed display modes\n"); SDL_Quit(); return 1; }
  } else { display.set_chunky(chunky != 0, chunky_cell); display.set_disp(use_disp);
    if (!linear) display.set_disp_grid(static_cast<u8>(((256 - grid) * 255) / 256)); display.set_fbdev(use_fbdev); if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout)) { SDL_Quit(); return 1; } }
  // A single-screen layout shows one screen: the core skips the other's
  // engine (Gpu::set_screen_visible). Every other layout, and dual-window,
  // shows both.
  auto apply_visibility = [&] {
    const Disp::Layout& l = display.current_layout();
    const bool single = !dual_window && l.mode == Disp::Mode::Single;
    for (int s = 0; s < 2; ++s) nds.gpu.set_screen_visible(s, !single || s == l.primary);
  };
  apply_visibility();

  ds::sdl::Audio audio;
  if (audio_on) audio.open(cfg.flag("audio.native_rate", true));
  audio.set_volume(cfg.num("audio.volume", 100));
  // Not during a replay: the log carries the mic, and an open capture device
  // would only add work to a measurement.
  ds::sdl::MicAlsa mic_alsa;
  // Opened lazily, on the game's first AUX read. Most titles never sample the
  // mic, and on the handhelds the capture PCM shares a DAI with the playback
  // stream SDL is holding: a failed setup there can leave playback not
  // consuming, which costs the frame pacer far more than the mic is worth.
  // `rejected` means the device is present but would not take our parameters;
  // opening it again through SDL would only poke the same codec twice.
  bool mic_tried = !(mic_on && !replay);
  auto open_mic = [&] {
    if (mic_tried) return;
    mic_tried = true;
    if (!mic_alsa.open(ds::spu::Spu::SAMPLE_RATE, cfg.str("audio.mic_dev").c_str()) && !mic_alsa.rejected())
      audio.open_capture();
  };

  ds::sdl::Input input;
  input.configure(cfg);
  input.open_controllers();
  std::vector<u32> menu_fb[2] = {std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H), std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H)};
  // Where a zipped game unpacks to when it cannot unpack beside its zip, how
  // much the cache may hold, and whether it survives the session
  // (cart/zip_cache.h; config [paths] cache, [cart] cache_mb, cache).
  nds.rom_cache_dir = cfg.str("paths.cache", "");
  nds.rom_cache_max_bytes = static_cast<u64>(std::max(0, cfg.num("cart.cache_mb", 2048))) << 20;
  const bool cache_session = cfg.str("cart.cache", "keep") == "session";
  // --clear-cache: every unpacked image goes, except the one this launch is
  // about to use, which would only be unpacked again.
  if (clear_cache) {
    std::string keep;
    { std::ifstream f(rom_path, std::ios::binary); u8 magic[4] = {}; f.read(reinterpret_cast<char*>(magic), 4);
      if (f.gcount() == 4 && ds::cart::is_zip(magic, 4)) keep = ds::cart::zip_cache_path(rom_path); }
    u64 freed = 0;
    std::vector<std::string> dirs = {ds::cart::zip_cache_dir(rom_dir_of(rom_path))};
    if (!nds.rom_cache_dir.empty()) dirs.push_back(nds.rom_cache_dir);
    if (!cfg.str("paths.games").empty()) dirs.push_back(ds::cart::zip_cache_dir(cfg.str("paths.games")));
    for (const std::string& d : dirs) freed += ds::cart::clear_cache(d, keep);
    std::fprintf(stderr, "cache: cleared %llu MB of unpacked games\n", static_cast<unsigned long long>(freed >> 20));
  }
  // `plain`: nearest scaling with none of the picture effects -- the pause
  // menu and the loader's notice are text pages, not a DS picture: the grid
  // dimmed their lines, chunky merged their glyphs and the seams blurred them.
  auto set_scale_targets = [&](const ds::sdl::Display::Target target[2], bool scaled, bool plain = false) {
    // At DS resolution (Display::effects_at_source) only chunky applies: the
    // grid would dim every pixel, seams and bilinear are the identity.
    const bool at_source = display.effects_at_source();
    for (int i = 0; i < 2; ++i) {
      if (!scaled) { nds.gpu.set_scale_target(i, ds::gpu::Gpu::ScaleTarget{}); continue; }
      if (plain) { nds.gpu.set_scale_target(i, ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun_plain}); continue; }
      nds.gpu.set_scale_target(i, ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun, at_source || !target[i].grid ? 256u : grid, display.chunky_on(i) ? chunky : static_cast<u8>(0), chunky_thresh,
                                                            at_source ? static_cast<u8>(0) : seam_blend, target[i].seam_w,
                                                            static_cast<const ds::gpu::Gpu::CellMap*>((dual_window && i == bottom_display ? display2 : display).cell_map(i)),
                                                            linear && !at_source, target[i].lin_sx, target[i].lin_wx});
    }
  };
  // Loads a ROM with the "unpacking" notice up if it takes more than a
  // moment -- a zipped game's first launch writes the whole image to the
  // card. The load runs on a worker; this thread pumps events (B cancels,
  // quit cancels) and redraws the notice over the dimmed held frame the way
  // the pause menu is drawn. Nothing on the panel is a forecast: the dots
  // advance only when the extraction reports progress, so they prove the
  // worker is alive rather than that time is passing, and after a while
  // with no progress the line says so.
  auto load_rom_notice = [&](const std::string& path) -> bool {
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<u64> progress{0};
    bool ok = false;
    nds.rom_cancel = &cancel;
    nds.rom_progress = [](void* u, u64 bytes, u64) { static_cast<std::atomic<u64>*>(u)->store(bytes); };
    nds.rom_progress_user = &progress;
    std::thread worker([&] { ok = nds.load_rom(path); done = true; });
    const std::string title = rom_stem(base_name(path));
    const Uint32 start = SDL_GetTicks();
    Uint32 last_change = start, last_step = start;
    u64 last_progress = 0;
    int dots = 0;
    bool shown = false;
    while (!done) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
      if (input.quit() || g_signalled || ((input.take_menu_presses() >> ds::io::Io::Button::BTN_B) & 1)) cancel = true;
      const Uint32 now = SDL_GetTicks();
      const u64 p = progress.load();
      if (p != last_progress) {
        last_progress = p; last_change = now;
        if (now - last_step >= 250) { dots = (dots + 1) % 4; last_step = now; }
      }
      if (!shown && now - start < 300) { SDL_Delay(10); continue; }   // a loose or cached game never shows it
      if (!shown) display.set_page(true);
      shown = true;
      const int menu_screen = dual_window ? 0 : display.current_layout().primary;
      const u32* fb[2];
      for (int i = 0; i < 2; ++i) {
        std::memcpy(menu_fb[i].data(), nds.gpu.framebuffer(i), menu_fb[i].size() * 4);
        ds::sdl::dim_framebuffer(menu_fb[i].data(), static_cast<u32>(menu_fb[i].size()));
        fb[i] = menu_fb[i].data();
      }
      char line2[32];
      if (cancel) std::snprintf(line2, sizeof line2, "STOPPING");
      else if (now - last_change > 5000) std::snprintf(line2, sizeof line2, "WAITING ON THE CARD");
      else std::snprintf(line2, sizeof line2, "UNPACKING%.*s", dots, "...");
      ds::sdl::draw_notice(ds::sdl::Blit{menu_fb[menu_screen].data(), ds::SCREEN_W, ds::SCREEN_H, nullptr},
                           title.c_str(), line2, "FIRST LAUNCH ONLY    B CANCELS");
      ds::sdl::Display::Target target[2] = {};
      bool scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      if (scaled) {
        set_scale_targets(target, true, true);
        for (int i = 0; i < 2; ++i) nds.gpu.scale_image(i, menu_fb[i].data());
        display.end_frame();
        if (dual_window) display2.end_frame();
        set_scale_targets(target, false);
      } else {
        display.draw(fb);
        if (dual_window) display2.draw(fb);
      }
      SDL_Delay(50);
    }
    worker.join();
    if (shown) display.set_page(false);
    nds.rom_cancel = nullptr; nds.rom_progress = nullptr; nds.rom_progress_user = nullptr;
    if (!ok && cancel) VLOG("unpacking cancelled\n");
    return ok;
  };
  // Session mode: the image is for this run only.
  auto discard_session_cache = [&] {
    if (cache_session && !nds.rom_cache_path.empty()) { ds::cart::remove_cached(nds.rom_cache_path); nds.rom_cache_path.clear(); }
  };
  // The ROM goes in after the display is open, so a zipped game's first
  // launch -- which unpacks it to the card, seconds to a minute -- can show
  // the notice instead of a black panel.
  if (!boot_firmware && !load_rom_notice(rom_path)) { std::fprintf(stderr, "could not read %s\n", rom_path.c_str()); SDL_Quit(); return 1; }
  // On a firmware boot the loader cart goes in the slot. A BootMenu.nds beside
  // the config wins if there is one -- that is how a hand-made card from
  // tools/mkcart.py is used -- and otherwise the built-in one is assembled in
  // memory, so the emulator needs no file shipped alongside it. Its two banner
  // lines are the only part worth configuring; a different icon means building
  // a card with the script.
  if (boot_firmware) {
    if (nds.load_rom(rom_path.c_str())) {
      VLOG("loader cart: %s\n", rom_path.c_str());
    } else if (cfg.flag("loader.card", true) &&
               nds.load_rom_image(ds::sdl::build_loader_cart(cfg.str("loader.title", "Game Menu"),
                                                             cfg.str("loader.subtitle", "Dariragan! Dagozuban!")))) {
      VLOG("loader cart: built in\n");
    } else {
      VLOG("loader cart: none; the slot stays empty\n");
    }
  }
  // The per-game file goes on top of the global one, the command line on top of both.
  // Title ID first, then the ROM's filename, so the file named like the ROM
  // wins; that is also where hotkey-picked settings are remembered.
  if (nds.cart) {
    for (const std::string& p : {ds::sdl::Config::game_path_code(nds.cart->header().game_code), ds::sdl::Config::game_path_rom(rom_path)})
      if (!p.empty() && cfg.load(p)) VLOG("config: %s\n", p.c_str());
    apply_cli();
    VLOG("game: %.12s [%.4s]\n", nds.cart->header().game_title, nds.cart->header().game_code);
    // Which entry a zip was read from -- the interesting case is an archive
    // holding more than one, where the pick is worth being able to check.
    if (!nds.rom_zip_entry.empty()) VLOG("zip: %s\n", nds.rom_zip_entry.c_str());
  }
  // Everything keyed to the ROM in the slot -- saves, states, screenshots,
  // cheats -- lives here, so that launching a game from the loader cart can
  // re-derive the lot rather than keep writing the loader's.
  Session session;
  session.open(nds, cfg, rom_path, save_arg);

  nds.sched.set_quantum(quantum);
  nds.gpu3d.set_timing_oc(cfg.flag("emu.timing_oc", false));
  nds.io.set_cart_bulk(cfg.flag("emu.fast_load", false));   // may introduce accuracy issues, see config.cpp
  nds.gpu3d.renderer().set_aa(cfg.flag("video.aa", false));   // opt-in: see config.cpp
  if (!boot_firmware) nds.setup_direct_boot();
  // A real console's clock, seeded from this machine. Off in the core by
  // default so the verification harness stays reproducible; a frontend
  // showing someone their own DS menu wants the real date on it. Not under
  // --replay: a recorded scene has to reproduce frame for frame, and a game
  // that reads the date (Animal Crossing, the Pokemon day/night cycle) would
  // otherwise play differently every time it was replayed.
  if (!replay || rtc_host) nds.io.start_rtc_clock();
  else VLOG("rtc: frozen for the replay\n");
  if (replay && rtc_host) std::fprintf(stderr, "rtc: --rtc-host over a replay; this run is not reproducible\n");
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
  if (jit && cfg.flag("emu.cpu_oc", false)) ds::jit::set_cpu_oc(true);   // see config.cpp; translate-time pricing, so before the first block
  // The firmware boots under per-instruction budget checks. Block-granularity
  // overshoot has been seen to stop it booting at all on the RG DS -- not
  // every time, which is what a timing race looks like -- and the console is
  // idle enough there that the cost of checking does not show. It is dropped
  // again the moment a game is launched, where it very much would.
  if (jit && boot_firmware) { ds::jit::set_strict(true); VLOG("jit: strict timing for the firmware\n"); }
#else
  (void)jit;
#endif
  // A replay is a measurement, not a play session: it must start from the
  // same battery save every time or it is not reproducible, and writing back
  // would mean the second run of a scene no longer matches the first. The headless
  // frontend has always loaded --save read-only for this reason; match it here, and
  // take an explicit --save too so both frontends can be pointed at the same
  // scene save rather than one silently picking up <rom>.sav.
  load_save(nds, session.sav);
  const bool save_readonly = replay != nullptr;
  if (save_readonly) VLOG("save: read-only for the replay\n");

  ds::input::Log log;
  if (record && replay) { std::fprintf(stderr, "--record and --replay are exclusive\n"); return 2; }
  if (record && !log.open_write(record)) { std::fprintf(stderr, "cannot write %s\n", record); return 1; }
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    VLOG("replay: %u frames from %s\n", log.frames(), replay);
  }
  // After the battery save, so a state's SRAM wins over <rom>.sav, and after
  // the replay log is open so it can be wound forward to the state's frame.
  if (load_state) {
    bool got_layout = false;
    if (!load_state_file(nds, load_state, layout, got_layout)) return 1;
    // The state's view replaces the config's, dual-window aside (two panels
    // show both screens, whatever the state says).
    if (got_layout && !dual_window) { display.set_layout(layout); apply_visibility(); }
    // A replay continues from the state's frame, not from the log's start.
    if (log.reading()) { ds::input::Frame f; for (u64 k = 0; k < nds.frame_count && log.read(f); ++k) {} }
  }
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
  // The on-screen counter. It reads the same 60-frame measurement the DS_FPS
  // log line does -- presented frames per second, so frameskip and fast
  // forward are visible in the number -- and holds the last value between
  // measurements rather than blinking. [video] fps starts it on; the `fps`
  // hotkey toggles it and is unbound by default.
  bool fps_osd = cfg.flag("video.fps", false);
  int fps_value = 0;                        // last measured, 0..999
  Uint64 fps_mark = SDL_GetPerformanceCounter();
  Uint64 emu_ticks = 0, draw_ticks = 0;
  u64 frames = 0;
  // Per-frame emulation time, for the same report the headless frontend prints. Only the
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
  // Frameskip. [emu] frameskip is the limit N: at most N frames in a row are
  // skipped, so at least one in N+1 is drawn. "fixed" always skips exactly
  // that pattern; "adaptive" (the default) skips only while the emulator is
  // behind real time, which is measured as a debt in milliseconds -- how far
  // the work of the frames so far has run over their budget -- and pays the
  // debt down by the time a skipped frame saves.
  //
  // A skipped frame is not merely un-presented: the core leaves out both
  // engines' line rendering, the 3D raster feeding it, and the scaling, while
  // everything the guest can observe still runs (see Gpu::set_frame_skip).
  // The decision has to reach the core one frame early, because the 3D raster
  // for a frame runs at line 215 of the frame before it; will_skip_frame()
  // reports what the frame about to run will actually do (a frame that
  // display-captures is never skipped, whatever the policy asks for).
  const int fs_limit = cfg.num("emu.frameskip", 0);
  const bool fs_adaptive = cfg.str("emu.frameskip_mode", "adaptive") != "fixed";
  const bool fs_capture = cfg.flag("emu.frameskip_capture", false);
  nds.gpu.set_frameskip_capture(fs_capture);
  u64 fs_refused = 0;         // skips the core would not take (capture / display FIFO)
  const double frame_budget_ms = frame_ns / 1e6;
  // Skipping runs in blocks of a whole display period (see
  // Gpu::display_phase_period), and so does drawing: a game that renders one
  // screen per frame and swaps them needs every phase of a period drawn, or
  // each presented frame has one fresh screen and one several frames old, and
  // which one alternates -- the two screens look like they are swapping.
  int fs_left = 0;            // frames left in the current block
  bool fs_in_skip = false;    // that block is a skip block
  int fs_blocks = 0;          // skip blocks run back to back, against the limit
  bool fs_period_warned = false;
  int fs_drawn_run = 0;       // drawn frames since the last skipped one
  u64 fs_skipped = 0;         // reported with the frame statistics
  double fs_debt_ms = 0;      // adaptive: how far behind real time we are
  // Auto-save: one state written to the unlisted ".auto" slot when the
  // session ends, so a launcher's kill or a Ctrl-C can be resumed with
  // --load-state. Nothing is written while playing, so it costs no frame time.
  const bool autosave = cfg.flag("emu.autosave", false);
  // Its thumbnail: "true" puts <GAMECODE>.auto.png beside the state, any
  // other value is the file to write (a launcher names the picture its game
  // switcher looks for). Only taken when the state is written.
  const std::string autosave_png_cfg = cfg.str("emu.autosave_png", "false");
  const bool autosave_png = autosave_png_cfg != "false" && autosave_png_cfg != "0" && !autosave_png_cfg.empty();
  bool ff_toggle = cfg.flag("emu.fast_forward", false);
  const int ff_speed = cfg.num("emu.ff_speed", 0), ff_skip = cfg.num("emu.ff_skip", 3);
  bool was_fast = false;
  std::vector<u32> cursor_fb(ds::SCREEN_W * ds::SCREEN_H);   // bottom screen with the pen crosshair
  std::vector<u32> osd_fb(ds::SCREEN_W * ds::SCREEN_H);      // the primary screen with the slot digit / FPS counter
  std::vector<u32> flash_fb[2] = {std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H), std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H)};   // both screens under the screenshot flash
  int flash_left = 0;                                        // frames of screenshot flash still to show
  int slot_shown = 0;                                        // frames left to show the slot digit
  // PiP inset opacity: where it is now (0..255), frames of opacity left
  // after the last touch, and the hold length from the config.
  const int pip_touch_hold = std::max(0, cfg.num("video.pip_touch_hold", 60));
  constexpr int PIP_FADE_STEP = 24;                          // ~10 frames rest to opaque
  int pip_alpha = static_cast<int>(layout.pip_alpha * 255.0 + 0.5), pip_hold = 0;
  // The pause menu (menu.h) and the two screen copies it is composited into.
  ds::sdl::Menu menu;
  menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
  session.load_enabled(nds);
  // The library the loader cart's picker offers. Only read when there is a
  // loader cart to raise it: a normal session never shows the list.
  const std::vector<ds::sdl::Menu::GameEntry> games =
      boot_firmware && nds.cart ? enumerate_games(cfg.str("paths.games")) : std::vector<ds::sdl::Menu::GameEntry>{};
  menu.set_games(&games);
  // Armed until a game is launched: after that the cart in the slot is a real
  // one, and its own reads at its own arm9_rom_offset mean nothing.
  bool launcher = boot_firmware && nds.cart != nullptr;
  if (launcher) VLOG("launcher: %zu games in %s\n", games.size(), cfg.str("paths.games").c_str());
  bool launching = false;       // the card's launch fade is on screen; the list is going up
  bool launch_latched = false;  // ... and it has already been raised once for this fade
  bool menu_dirty = false;      // the menu screens need compositing and presenting again
  Uint32 menu_ms = 0;           // SDL_GetTicks at the menu's last tick
  // Pausing waits for one more presented, *unscaled* frame. The fast scaling
  // path has the GPU write its lines straight into the window surface and
  // never fills fb_ (Gpu::output_engine), so stopping the moment the hotkey
  // arrives would leave the menu with nothing current to draw over. Deferring
  // costs a frame nobody can see and guarantees a real picture underneath.
  bool pause_pending = false;
  // The screenshot hotkey works the same way, for the opposite reason: the
  // scanline tiers' buffers carry the grid, chunky and seams, and a
  // screenshot wants the game's own picture, laid out but unadorned. So it
  // is taken from fb_ after one deliberately unscaled frame, not read back
  // from the panel.
  bool shot_pending = false;
  auto refresh_slots = [&] { for (int i = 0; i < 10; ++i) {
    FILE* f = std::fopen(state_path(nds, session.states_dir, i).c_str(), "rb");
    menu.set_slot_used(i, f != nullptr);
    if (f) std::fclose(f);
  } };
  // Battery save flush: once the chip has been quiet for a second, and at
  // every point a session could end (pause, lid, quit).
  u32 sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
  u64 sram_quiet_since = 0;
  auto flush_save = [&] { if (!save_readonly && nds.cart && nds.cart->sram_dirty()) write_save(nds, session.sav); };
  // The session is ending: leave a state behind. The same two guards the
  // save-state hotkey carries -- a replay must not write, and a recording is
  // the inputs from boot, so a state alongside it would only mislead.
  // Whether fb_ holds the picture of the last frame run: it does after an
  // unscaled frame (the pause stop, a screenshot frame, the renderer tier)
  // and not after a scanline-tier frame, whose lines went to the panel.
  bool fb_current = false;
  auto autosave_now = [&] {
    if (!autosave || save_readonly || log.writing()) return;
    // The thumbnail is the game's picture without the panel effects, like a
    // screenshot, so it comes from fb_. When the last frame went to the
    // panel instead, run one more, unscaled, before the state is taken, so
    // the two agree: a frame nobody sees, at a moment the session is ending.
    if (autosave_png && !fb_current) {
      ds::sdl::Display::Target none[2] = {};
      set_scale_targets(none, false);
      nds.run_frame();
      fb_current = true;
    }
    if (!save_state_file(nds, auto_state_path(nds, session.states_dir), display.current_layout())) return;
    flush_save();   // the .sav and the state never diverge
    if (!autosave_png) return;
    const bool beside = autosave_png_cfg == "true" || autosave_png_cfg == "1";
    std::string png = autosave_png_cfg;
    if (beside) { png = auto_state_path(nds, session.states_dir); png.replace(png.size() - 3, 3, "png"); }
    if (write_png(nds, png, display.current_layout())) std::fprintf(stderr, "state: thumbnail %s\n", png.c_str());
  };
  // A loaded state's view, applied the way the layout hotkeys apply theirs.
  Disp::Layout loaded_layout = layout; bool got_layout = false;   // from `layout`: a state carries no pip_alpha, the config's stays
  auto apply_loaded_layout = [&] {
    if (!got_layout || dual_window) return;
    display.set_layout(loaded_layout);
    apply_visibility();
    menu_dirty = true;
  };
  auto set_paused = [&](bool p) {
    if (p == paused) return;
    paused = p;
    audio.pause(p);
    display.set_page(p);
    if (p) flush_save(); else { next_frame = SDL_GetPerformanceCounter(); fs_debt_ms = 0; }
    VLOG("%s\n", p ? "paused" : "resumed");
  };
  while (!input.quit() && !g_signalled && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
    for (ds::sdl::Action a : input.take_actions()) {
      using A = ds::sdl::Action;
      switch (a) {
      case A::Pause:
        if (paused) {
          state_slot = menu.slot();
          if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
          menu.set_open(false);
          set_paused(false);
        }
        else pause_pending = true;
        break;
      case A::VolumeUp: audio.set_volume(audio.volume() + 10); audio.set_muted(false); VLOG("volume %d%%\n", audio.volume()); break;
      case A::VolumeDown: audio.set_volume(audio.volume() - 10); VLOG("volume %d%%\n", audio.volume()); break;
      case A::Mute: audio.set_muted(!audio.muted()); VLOG("%s\n", audio.muted() ? "muted" : "unmuted"); break;
      // Anything that moves the screens around while the pause menu is up has
      // to recomposite it: nothing else redraws until the menu itself changes.
      case A::Fullscreen: display.toggle_fullscreen(); if (dual_window) display2.toggle_fullscreen(); menu_dirty = true; break;
      case A::LayoutNext: case A::LayoutPrev: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        const int n = static_cast<int>(layout_cycle.size());
        int at = 0;
        for (int i = 0; i < n; ++i) if (layout_cycle[static_cast<size_t>(i)] == l.mode) { at = a == A::LayoutNext ? (i + 1) % n : (i + n - 1) % n; break; }
        l.mode = layout_cycle[static_cast<size_t>(at)];
        display.set_layout(l);
        apply_visibility();
        menu_dirty = true;
        VLOG("layout: %s\n", Disp::mode_name(l.mode));
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.layout", Disp::mode_name(l.mode));
        break;
      }
      case A::ScreenSwap: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.primary = 1 - l.primary;
        display.set_layout(l);
        apply_visibility();
        menu_dirty = true;
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.screen", l.primary ? "bottom" : "top");
        break;
      }
      case A::PipCornerNext: {
        if (dual_window) break;
        Disp::Layout l = display.current_layout();
        l.corner = static_cast<Disp::Corner>((static_cast<int>(l.corner) + 1) % static_cast<int>(Disp::Corner::Count));
        display.set_layout(l);
        menu_dirty = true;
        if (!session.game_ini.empty()) ds::sdl::Config::store(session.game_ini, "video.pip_corner", Disp::corner_name(l.corner));
        break;
      }
      case A::Screenshot: if (paused) screenshot(nds, session.shots_dir, display.current_layout()); else shot_pending = true; break;
      case A::Lid: input.set_lid(!input.lid()); VLOG("lid: %s\n", input.lid() ? "closed" : "open"); if (input.lid()) flush_save(); break;
      case A::SlotNext: state_slot = (state_slot + 1) % 10; slot_shown = 90; VLOG("state slot %d\n", state_slot); break;
      case A::SlotPrev: state_slot = (state_slot + 9) % 10; slot_shown = 90; VLOG("state slot %d\n", state_slot); break;
      case A::SaveState:
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (save_state_file(nds, state_path(nds, session.states_dir, state_slot), display.current_layout())) flush_save();   // the .sav and the state never diverge
        break;
      case A::LoadState:
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        // A recording is the inputs from boot; a load would leave it unreplayable.
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        if (load_state_file(nds, state_path(nds, session.states_dir, state_slot), loaded_layout, got_layout)) {
          apply_loaded_layout();
          audio.clear();
          next_frame = SDL_GetPerformanceCounter();
          fs_debt_ms = 0;
          flush_save();
        }
        break;
      // Without DS_FPS the measurement only runs while the counter is on, so
      // fps_mark is stale by however long it was off: restart the window, or
      // the first number shown would average over that whole gap.
      case A::FpsToggle:
        fps_osd = !fps_osd;
        if (fps_osd && !show_fps) { fps_mark = SDL_GetPerformanceCounter(); emu_ticks = draw_ticks = 0; }
        break;
      case A::FastForwardToggle: ff_toggle = !ff_toggle; VLOG("fast forward %s\n", ff_toggle ? "on" : "off"); break;
      default: break;
      }
    }
    if (paused) {
      // Nothing runs behind the menu, so it is composited only when something
      // about it changed -- otherwise this is a plain idle tick.
      if (menu.open()) {
        // The menu is ticked every idle pass, not only when a button moves:
        // holding a direction repeats, and a cheat name too long for its row
        // scrolls, both of which need to know how much time has gone by.
        const Uint32 now_ms = SDL_GetTicks();
        const u32 elapsed = static_cast<u32>(now_ms - menu_ms);
        menu_ms = now_ms;
        switch (menu.update(input.take_menu_presses(), input.menu_held(), elapsed)) {
        case Menu::Result::None: break;
        case Menu::Result::Resume:
          state_slot = menu.slot();
          if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
          menu.set_open(false);
          set_paused(false);
          break;
        // Both carry the same guards as the save-state hotkeys: a replay must
        // stay the run it recorded, and a recording is the inputs from boot,
        // which a load would leave unreplayable.
        case Menu::Result::Save:
          if (save_readonly) std::fprintf(stderr, "state: not during a replay\n");
          else if (save_state_file(nds, state_path(nds, session.states_dir, menu.slot()), display.current_layout())) flush_save();
          refresh_slots();
          break;
        case Menu::Result::Load:
          if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
          if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
          // A load replaces the picture the menu is drawn over, so it also
          // leaves the menu: the player wants to see where they landed.
          if (load_state_file(nds, state_path(nds, session.states_dir, menu.slot()), loaded_layout, got_layout)) {
            apply_loaded_layout();
            state_slot = menu.slot();
            menu.set_open(false);
            set_paused(false);
            audio.clear();
            next_frame = SDL_GetPerformanceCounter();
            fs_debt_ms = 0;
            flush_save();
          } else refresh_slots();
          break;
        case Menu::Result::Launch: {
          const std::string pick = menu.chosen();
          VLOG("launcher: %s\n", pick.c_str());
          flush_save();
#if DSPERATE_JIT
          // Back off the firmware's strict timing: the game wants the speed,
          // and the environment override still wins if it was asked for.
          if (jit) { ds::jit::set_strict(std::getenv("DS_JIT_STRICT") != nullptr); ds::jit::flush_all(); }
#endif
          nds.reset();
          discard_session_cache();
          if (!load_rom_notice(pick)) {
            // Stay on the list rather than reset into nothing: another game
            // in the same directory may well be readable, and a cancelled
            // unpacking is a change of mind, not an error.
            std::fprintf(stderr, "launcher: could not read %s\n", pick.c_str());
            break;
          }
          nds.setup_direct_boot();
          // Everything keyed to the ROM follows it, or the game would go on
          // writing the loader's saves, states, screenshots and cheats under
          // the loader's game code. --save is deliberately not carried over:
          // it pins one file, and it was given for the ROM on the command
          // line, not for whatever the player picks here.
          launcher = false;
          session.open(nds, cfg, pick, nullptr);
          menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
          session.load_enabled(nds);
          load_save(nds, session.sav);
          sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
          state_slot = 0;
          refresh_slots();
          menu.set_open(false);
          set_paused(false);
          break;
        }
        case Menu::Result::Quit: input.request_quit(); break;
        }
      }
      if (menu.open() && (menu_dirty || menu.dirty())) {
        menu_dirty = false;
        menu.clear_dirty();
        // The menu goes on the DS top screen in dual-window mode (`display`
        // is opened with only_screen 0, so it is the top one whichever
        // physical output it landed on), and on the layout's primary screen
        // otherwise -- in a Single layout that is the only screen drawn, and
        // the other engine is switched off, so its framebuffer is stale.
        const int menu_screen = dual_window ? 0 : display.current_layout().primary;
        const u32* src[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        const u32* fb[2];
        for (int i = 0; i < 2; ++i) {
          std::memcpy(menu_fb[i].data(), src[i], menu_fb[i].size() * 4);
          // Both screens dim: the one the menu is not on is how a glance says
          // the machine is stopped, and the one it is on gives the panel its
          // contrast.
          ds::sdl::dim_framebuffer(menu_fb[i].data(), static_cast<u32>(menu_fb[i].size()));
          fb[i] = menu_fb[i].data();
        }
        menu.draw(ds::sdl::Blit{menu_fb[menu_screen].data(), ds::SCREEN_W, ds::SCREEN_H, nullptr});
        // The menu has to reach the screen the same way a frame does. On the
        // scanline tiers (window surface, dmabuf, scanout) Display has no
        // renderer at all -- open() returns before creating one -- so draw()
        // would present nothing, which is what a paused screen looked like.
        // There are no display lines to scale here, so the whole image goes
        // through the scaler in one go.
        ds::sdl::Display::Target target[2] = {};
        bool scaled = display.begin_frame(target);
        if (dual_window) scaled = display2.begin_frame(target) && scaled;
        if (scaled) {
          set_scale_targets(target, true, true);
          for (int i = 0; i < 2; ++i) nds.gpu.scale_image(i, menu_fb[i].data());
          display.end_frame();
          if (dual_window) display2.end_frame();
          // Nothing may keep pointing into a buffer the display just released.
          set_scale_targets(target, false);
        } else {
          display.draw(fb);
          if (dual_window) display2.draw(fb);
        }
      }
      SDL_Delay(10);
      continue;
    }
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
        if (nds.io.mic_used()) open_mic();
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
    // The policy decides for the frame after the one about to run; what the
    // core settled for this one is what governs the present.
    if (fs_limit > 0) {
      const int period = nds.gpu.display_phase_period();
      // The limit counts blocks, not frames: on a game whose screens take a
      // whole period to come round, `frameskip = 3` means three of those
      // periods skipped for one drawn, the same ratio a period-1 game gets.
      const int max_blocks = fs_limit;
      if (period > 1 && !fs_period_warned) {
        fs_period_warned = true;
        std::fprintf(stderr, "frameskip: this game drives its screens over %d frames, so it skips %d of every %d\n",
                     period, fs_limit * period, (fs_limit + 1) * period);
      }
      if (fs_left == 0) {                         // a block ended: pick the next one
        // Fixed skips its blocks whatever the clock says; adaptive skips only
        // while it is behind, and both stop at the limit and draw a period.
        const bool want = !fs_adaptive || fs_debt_ms > frame_budget_ms * 0.5;
        const bool skip = max_blocks > 0 && want && fs_blocks < max_blocks;
        fs_blocks = skip ? fs_blocks + 1 : 0;
        fs_in_skip = skip;
        fs_left = period;
      }
      --fs_left;
      nds.gpu.set_frame_skip(fs_in_skip);
    }
    const bool skipped = nds.gpu.will_skip_frame();
    // The first drawn frames of a block are not presented when the game needs
    // a whole period to come round: on Golden Sun each frame renders one
    // screen and leaves the other to the capture the next frame reads, so the
    // first frame after a skip has one screen fresh and one from before the
    // skip -- presenting it flashes the stale screen. Drawing the block
    // through and presenting its last frame shows both screens of one moment.
    // A run of drawn frames longer than a period presents every frame, so
    // adaptive that stops skipping goes straight back to full rate.
    const int fs_period = fs_limit > 0 ? nds.gpu.display_phase_period() : 1;
    if (skipped) { ++fs_skipped; fs_drawn_run = 0; } else ++fs_drawn_run;
    const bool fs_partial = fs_period > 1 && fs_drawn_run < fs_period;
    // Say why frameskip is doing nothing, once, rather than leaving it to look
    // like a broken setting: on a game that captures every frame the exact
    // policy has nothing it may skip.
    if (!skipped && fs_in_skip && ++fs_refused == 120 && !fs_capture)
      std::fprintf(stderr, "frameskip: this game display-captures its frames, which cannot be skipped exactly;\n"
                           "           --frameskip-capture (or [emu] frameskip_capture) skips them anyway\n");
    const bool present = pause_pending || shot_pending ? !skipped
                                       : (!skipped && !fs_partial && (!fast || ff_skip <= 0 || frames % static_cast<u64>(ff_skip + 1) == 0));
    // A translucent PiP inset comes up to opaque while the bottom screen is
    // in use -- the frame the console gets has a touch (finger, mouse, pen
    // button, or a replayed one), or the pad-driven pen is showing -- and
    // holds there for pip_touch_hold frames after the last touch before
    // fading back. Ramped so neither edge pops. Only when the inset IS the
    // bottom screen (top primary): with the bottom screen large, a touch
    // says nothing about the top screen sitting in the corner.
    {
      const Disp::Layout& l = display.current_layout();
      const int rest = static_cast<int>(l.pip_alpha * 255.0 + 0.5);
      const bool inset_is_bottom = l.mode == Disp::Mode::Pip && l.primary == 0;
      if (inset_is_bottom && (in.down || input.stylus_visible())) pip_hold = pip_touch_hold;
      else if (pip_hold > 0) --pip_hold;
      const int want = pip_hold > 0 && pip_touch_hold > 0 ? 255 : rest;
      pip_alpha += std::clamp(want - pip_alpha, -PIP_FADE_STEP, PIP_FADE_STEP);
      display.set_inset_alpha(static_cast<u8>(pip_alpha));
    }
    ds::sdl::Display::Target target[2] = {};
    bool scaled = false;
    if (present && !pause_pending && !shot_pending) {
      scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
    }
    set_scale_targets(target, scaled);
    fb_current = present && !scaled;   // a skipped frame renders nothing

    const Uint64 t0 = SDL_GetPerformanceCounter();
    nds.run_frame();

    // The console has switched itself off. On a firmware boot that is the
    // firmware leaving its settings pages -- the flash writes that saved them
    // have already landed in the image, so this is the moment to put them on
    // disk, and then to start the console again, which is what pressing the
    // power button would do next. A game reaching here is left running: it is
    // not expected to, and quitting on one stray write would lose more than
    // it saved.
    if (nds.power_off) {
      if (boot_firmware) {
        std::string err;
        if (nds.firmware_override_dirty() && !nds.save_firmware_override(fw_override, err))
          std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
        else if (nds.firmware_override_dirty())
          std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
        std::fprintf(stderr, "power off: rebooting the firmware\n");
        autosave_now();       // the reset below discards the session
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();          // clears power_off, and re-seeds the clock
      } else {
        nds.power_off = false;
      }
    }

    const Uint64 t1 = SDL_GetPerformanceCounter();
    if (present) {
      const bool cursor = input.stylus_visible() && !log.reading();
      // The crosshair is drawn in DS pixels, so on a bottom screen shown
      // small (the PiP inset, the dominant layouts' secondary) it shrinks
      // with the view; scale it back up by the view's reduction so it stays
      // the same size on the panel and findable in the corner.
      int cursor_size = input.stylus_size();
      {
        const Disp::Layout& cl = display.current_layout();
        double ratio = 1.0;
        if (!dual_window && cl.mode == Disp::Mode::Pip && cl.primary == 0) ratio = cl.pip;
        else if (!dual_window && (cl.mode == Disp::Mode::DominantV || cl.mode == Disp::Mode::DominantH) && cl.primary == 0) ratio = cl.dominant;
        if (ratio < 1.0) cursor_size *= std::clamp(static_cast<int>(1.0 / ratio + 0.5), 1, 4);
      }
      const bool slot_osd = slot_shown > 0;
      if (slot_shown > 0) --slot_shown;
      // The flash's alpha this frame: full white first, then straight down.
      const u32 flash_alpha = flash_left > 0 ? static_cast<u32>(255 * flash_left / FLASH_FRAMES) : 0;
      if (flash_left > 0) --flash_left;
      // Which screen the overlays land on. Layout::primary is the one shown
      // alone (Single), large (PiP) or dominant, so following it keeps them
      // where the player is looking -- and, in Single with screen = bottom,
      // visible at all: the other screen's view is not shown, so anything
      // drawn there goes into a side buffer that is never presented. Dual
      // window has no primary; both panels are shown, so the top screen it is.
      const Disp::Layout& osd_l = display.current_layout();
      const int osd_screen = dual_window ? 0 : osd_l.primary;
      // The PiP inset is blitted over the primary screen after this, so an
      // overlay in the inset's corner would be buried. Only the two top
      // corners are contested -- the slot digit sits top-left, the counter
      // top-right -- so the one whose corner the inset takes moves down its
      // own edge. Nothing else in the layout puts a screen in a corner: the
      // dominant modes lay the secondary out as a strip.
      const bool pip = !dual_window && osd_l.mode == Disp::Mode::Pip;
      const bool inset_top = pip && (osd_l.corner == Disp::Corner::TopLeft || osd_l.corner == Disp::Corner::TopRight);
      const bool inset_left = osd_l.corner == Disp::Corner::TopLeft || osd_l.corner == Disp::Corner::BottomLeft;
      const bool slot_bottom = inset_top && inset_left, fps_bottom = inset_top && !inset_left;
      if (scaled) {
        if (cursor) draw_cursor(CursorDst{target[1].px, target[1].pitch, target[1].h, target[1].xrun}, input.stylus_x(), input.stylus_y(), cursor_size);
        const CursorDst od{target[osd_screen].px, target[osd_screen].pitch, target[osd_screen].h, target[osd_screen].xrun};
        if (slot_osd) draw_slot(od, state_slot, slot_bottom);
        if (fps_osd) draw_number(od, fps_value, 3, true, fps_bottom);
        if (flash_alpha) for (int i = 0; i < 2; ++i) if (target[i].px) draw_flash(CursorDst{target[i].px, target[i].pitch, target[i].h, target[i].xrun}, flash_alpha);
        display.end_frame();
        if (dual_window) display2.end_frame();
      } else {
        const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        if (cursor) {
          std::memcpy(cursor_fb.data(), fb[1], cursor_fb.size() * 4);
          draw_cursor(CursorDst{cursor_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, input.stylus_x(), input.stylus_y(), cursor_size);
          fb[1] = cursor_fb.data();
        }
        // After the cursor: when the overlays are on the bottom screen this
        // copies the frame that already has the crosshair in it, so both show.
        if (slot_osd || fps_osd) {
          std::memcpy(osd_fb.data(), fb[osd_screen], osd_fb.size() * 4);
          const CursorDst od{osd_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr};
          if (slot_osd) draw_slot(od, state_slot, slot_bottom);
          if (fps_osd) draw_number(od, fps_value, 3, true, fps_bottom);
          fb[osd_screen] = osd_fb.data();
        }
        // Last, over whatever the cursor and the overlays left: a screen
        // still pointing at the GPU's own buffer is copied out first.
        if (flash_alpha) for (int i = 0; i < 2; ++i) {
          if (fb[i] == nds.gpu.framebuffer(i)) { std::memcpy(flash_fb[i].data(), fb[i], flash_fb[i].size() * 4); fb[i] = flash_fb[i].data(); }
          draw_flash(CursorDst{const_cast<u32*>(fb[i]), ds::SCREEN_W, ds::SCREEN_H, nullptr}, flash_alpha);
        }
        display.draw(fb);
        if (dual_window) display2.draw(fb);
      }
    }
    audio.push(nds, fast);
    // An unscaled frame is in fb_: the picture the screenshot wants.
    if (shot_pending && present) {
      shot_pending = false;
      screenshot(nds, session.shots_dir, display.current_layout());
      flash_left = FLASH_FRAMES;   // from the next frame: this one is already on the panel and in the file
    }
    // The frame the menu will sit on is presented and, because it went down
    // the unscaled path, is in fb_ as well. Now it is safe to stop.
    if (pause_pending && present) {
      pause_pending = false;
      // Raising the picker and raising the pause menu are the same stop: a
      // real, unscaled frame has just been presented and is in fb_, which is
      // what either page is drawn over.
      if (launching) { launching = false; menu.open_games(); }
      else { menu.set_slot(state_slot); refresh_slots(); menu.set_open(true); }
      menu_dirty = true;
      menu_ms = SDL_GetTicks();
      set_paused(true);
    }
    // The loader cart's picker: the console's own launch fade is the signal.
    //
    // Launching the card is the only thing in the DS menu that drives both
    // engines' MASTER_BRIGHT to white (Gpu::screens_forced_white). PictoChat,
    // DS Download Play, the settings pages and the shutdown they end in never
    // touch it, and the white stretch of the firmware's own boot is white
    // pixels rather than a forced screen -- checked on all of them. So the
    // register alone says "the card was launched", with no tap, no rectangle
    // of the menu's layout to keep up to date, and no timeout: it catches the
    // launch whether the player tapped the panel or selected it with the
    // D-pad and A, which a tap test cannot see at all.
    //
    // The cart read at the loader's arm9_rom_offset would be better still and
    // Cart::launch_read() still offers it, but this firmware never issues it:
    // the fade lands and the cart bus stays silent for ever after (measured --
    // the ROMCTRL access count is identical either side of the launch), so
    // nothing downstream of the fade can be waited on.
    //
    // Latched, because the white stays up: the picker would otherwise go
    // straight back up the moment the player closed it.
    if (launcher) {
      const bool white = nds.gpu.screens_forced_white();
      if (white && !launch_latched) {
        launch_latched = true;
        launching = true;
        VLOG("launcher: the card was launched; raising the list\n");
      } else if (!white) {
        launch_latched = false;
      }
    }
    // Stop only once the fade is up. Asking the register rather than the
    // pixels is what lets the animation play on the normal (scaled) path: a
    // forced unscaled frame is needed only for the one frame the menu is
    // drawn over, and pause_pending is what asks for that.
    if (launching) pause_pending = true;
    const Uint64 t2 = SDL_GetPerformanceCounter();
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;
    draw_ticks_total += t2 - t1;
    if (static_cast<long>(frames) >= stats_from) {
      frame_ms.push_back(static_cast<double>(t1 - t0) * ticks_to_ms);
      work_ms.push_back(static_cast<double>(t2 - t0) * ticks_to_ms);
    }
    if (fs_adaptive && fs_limit > 0) {
      // Only real-time play has a budget to fall behind: unthrottled fast
      // forward is always "behind" and would skip to the limit forever.
      const double work = static_cast<double>(t2 - t0) * ticks_to_ms;
      fs_debt_ms += (fast && ff_speed <= 0) ? 0.0 : work - frame_budget_ms;
      if (fs_debt_ms < 0) fs_debt_ms = 0;
      const double cap = frame_budget_ms * (fs_limit + 1);
      if (fs_debt_ms > cap) fs_debt_ms = cap;   // a long stall must not buy a run of skips
    }
    ds::prof::frame_mark();   // marks the emu slice: the present is not in a stage, it lands in "untimed" of work_ms

    const Uint64 t3 = SDL_GetPerformanceCounter();
    bool on_the_clock = true;
    if (fast && ff_speed <= 0) {
      on_the_clock = false;    // unthrottled
    } else if (audio.active() && !fast) {
      audio.pace();
      // A device that accepts samples but never plays them is no clock at
      // all: pace() returns at once and we fall back to the wall clock
      // rather than paying its probe every frame.
      on_the_clock = audio.stalled();
      if (!on_the_clock) next_frame = SDL_GetPerformanceCounter();
    }
    if (on_the_clock) {
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

    if ((show_fps || fps_osd) && frames % 60 == 0) {   // DS_FPS=1 and/or the on-screen counter
      const Uint64 now = SDL_GetPerformanceCounter();
      const double secs = static_cast<double>(now - fps_mark) / SDL_GetPerformanceFrequency();
      const double to_ms = 1e3 / SDL_GetPerformanceFrequency() / 60.0;
      const double fps = secs > 0 ? 60.0 / secs : 0.0;
      fps_value = fps >= 999.0 ? 999 : static_cast<int>(fps + 0.5);
      if (show_fps) {
        std::fprintf(stderr, "%.1f fps (%.0f%%), emu %.1f ms, present %.1f ms, audio queued %.1f frames\n",
                     fps, 100.0 * fps / (ds::ARM9_CLOCK_HZ / double(ds::CYCLES_PER_FRAME)),
                     emu_ticks * to_ms, draw_ticks * to_ms, audio.queued_frames());
        if (fs_limit > 0) std::fprintf(stderr, "  frameskip: %llu frames skipped (%s, limit %d)\n",
                                       static_cast<unsigned long long>(fs_skipped), fs_adaptive ? "adaptive" : "fixed", fs_limit);
      }
      fps_mark = now;
      emu_ticks = draw_ticks = 0;
    }
  }

  autosave_now();
  flush_save();
  discard_session_cache();
  if (nds.firmware_override_dirty()) {
    std::string err;
    if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
    else std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
  }
  if (fs_limit > 0)
    VLOG("frameskip (%s, limit %d): %llu of %llu frames not drawn\n", fs_adaptive ? "adaptive" : "fixed", fs_limit,
         static_cast<unsigned long long>(fs_skipped), static_cast<unsigned long long>(frames));
  if (log.writing()) std::fprintf(stderr, "recorded %u frames to %s\n", log.frames(), record);
  // The per-frame timing statistics and the over-budget window histogram.
  // A measurement tool, not something a player wants at the end of every
  // session, so this frontend prints them only on request -- the headless
  // one, whose whole job is measuring, always does. DS_PROFILE implies it:
  // asking for the stage breakdown without the frame series it annotates
  // would give a table with nothing to read it against.
  if (std::getenv("DS_FRAME_STATS") || ds::prof::enabled) {
    // Emulation work only -- see frame_report.h. The two excluded costs are
    // named on their own line so a headless/SDL disagreement can be attributed.
    ds::frame_report(frame_ms);
    // The same statistics over emulation + present, which is what a missed
    // display frame actually is. Only worth reading with --no-vsync: with
    // vsync on the present blocks and the tail pins to the refresh.
    ds::frame_report(work_ms, "work");
    ds::prof::frame_breakdown(frame_ms);
    if (!frame_ms.empty())
      std::fprintf(stderr, "  (emulation only; excluded: present %.1f ms, pacing %.1f ms total over %zu frames)\n",
                   static_cast<double>(draw_ticks_total) * ticks_to_ms,
                   static_cast<double>(pace_ticks) * ticks_to_ms, frame_ms.size());
  }
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
