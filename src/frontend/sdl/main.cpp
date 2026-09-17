// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SDL2 frontend: direct boot, both screens stacked, sound, and input from a
// keyboard, a game controller or a touchscreen. Settings come from an INI
// file (config.h) with the command line on top; hotkeys cover what a
// handheld needs (volume, layout, screenshots, save states), and the pause
// key opens a blitted menu over the held frame (menu.h).
#include "core/nds.h"
#include "core/host_cores.h"
#include "core/cart/zip.h"
#include "core/cart/zip_cache.h"
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/input/input_log.h"
#include "core/state/state.h"
#include "core/io/dsi_nand_persist.h"
#include "core/io/dsi_title_install.h"
#include "core/io/dsi_nand_launch.h"
#include "core/cheat/database.h"
#include "core/cart/miniz/miniz_tdef.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#if DSPERATE_CHEEVOS
#include "cheevos/cheevos_client.h"
#include "cheevos/cheevos_hash.h"
#include "cheevos/cheevos_http.h"
#endif
#if DSPERATE_NET
#include "net/lan_mp.h"
#include "net/slirp_driver.h"
#include <arpa/inet.h>
#endif
#include "audio.h"
#include "config.h"
#include "display.h"
#include "input.h"
#include "lid.h"
#include "loader_cart.h"
#include "menu.h"
#include "cpu_gov.h"
#include "pacer.h"

#include <dirent.h>
#include <algorithm>
#include "mic_alsa.h"

#include <SDL2/SDL.h>
#include <sched.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <ctime>
#include <string>
#include <random>
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
    "  Without dumps a built-in replacement BIOS and a generated firmware run games\n"
    "  (direct boot only; the DS menu and exact timing need the real files).\n"
    "  --config F      settings file (default ~/.config/dsperate/dsperate.ini; every\n"
    "                  option below has a key there; games/<rom name>.ini and games/<CODE>.ini\n"
    "                  override it per game, the filename one winning)\n"
    "  --write-config F  write the default settings file (all keys commented) to F and exit\n"
    "  --dsi-mode      boot a DSi from its NAND (boot2, then the DSi Launcher) instead of the DS menu; no\n"
    "                  ROM. Needs --bios9i F --bios7i F (the DSi BIOS pair) and --dsi-nand F (a nand.bin\n"
    "                  with its nocash footer; paths.dsi_nand), with --bios9/--bios7 as usual and the DSi's\n"
    "                  firmware (--firmware, or paths.dsi_firmware for every DSi session).\n"
    "                  EXPERIMENTAL. The NAND is opened\n"
    "                  read-only; what a session changes is kept as files instead: title saves as\n"
    "                  <GAMECODE>.pub/.prv/.bnr (paths.saves, else beside the dump), system settings in\n"
    "                  <nand>.ovr and photos under <nand>.photos/, put back in at the next boot.\n"
    "                  A DSiWare .nds/.cia given with it is installed into the session (not the dump) unless\n"
    "                  the NAND has it; it needs its signed DSi TMD: embedded in the CIA, cached beside the\n"
    "                  saves, downloaded from Nintendo's update CDN, or --dsi-tmd F (default <game>.tmd).\n"
    "                  --dsi-offline never downloads; --dsi-hide-installed (emu.dsi_hide_installed) hides\n"
    "                  the dump's own DSiWare.\n"
    "                  The title starts straight away; --dsi-menu boots to the DSi Menu with it instead\n"
    "                  With no title, the loader card is in the slot there as on the DS menu: launching\n"
    "                  it raises the game list, and a DS game picked from it runs on a DS\n"
    "                  Without --dsi-nand, a DSiWare .nds/.cia given with it runs with only the DSi BIOS\n"
    "                  pair: the launcher's hand-off is emulated, the NAND and the DSi settings are made\n"
    "                  up from [user], and saves go to paths.saves (else beside the game). The DSi system\n"
    "                  font is DSperate's own (Noto Sans, WenQuanYi Micro Hei); --dsi-font F\n"
    "                  (paths.dsi_font) uses a console's /sys/TWLFontTable.dat instead.\n"
    "                  --dsi-sd DIR (paths.dsi_sd) puts a host folder in the DSi's SD card slot; what the\n"
    "                  DSi changes on the card is written back to the folder (new, changed and removed files)\n"
    "                  A DSiWare .nds/.dsi/.cia named without --dsi-mode runs this way too, and so does one\n"
    "                  picked from the loader's game list (marked [DSi]); the DSi BIOS pair can be set as\n"
    "                  paths.bios9i/paths.bios7i. When the title leaves (a soft reset), the session ends.\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) | horizontal | single | pip | dominant_v | dominant_h\n"
    "  --screen S      top (default) or bottom: the screen shown alone, large or dominant\n"
    "  --pip-alpha X   opacity of the PiP inset at rest, 0..1 (default 1; it comes up to opaque\n"
    "                  while the bottom screen is touched)\n"
    "  --dominant-ratio R  the dominant layouts' secondary, relative to the dominant screen: auto\n"
    "                  (default: the dominant screen takes the largest whole scale that leaves the\n"
    "                  secondary at least --dominant-threshold of it, and the secondary the room left)\n"
    "                  or a ratio such as 0.5\n"
    "  --dominant-threshold T  the smallest secondary auto accepts, 0.1..0.99 (default 0.25)\n"
    "  --integer-scale [M]  whole panel pixels per DS pixel: under (default when bare; the largest\n"
    "                  that fits, letterboxed) | over (the smallest that covers, cropped, keeping the\n"
    "                  edge between the screens) | off\n"
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
    "  --no-audio      run without sound\n"
    "  --volume N      0..100\n"
    "  --audio-buffer X  how much sound is held ahead: auto (the default) or milliseconds;\n"
    "                  audio.buffer_size. Lower is less delay and less slack before a\n"
    "                  late frame is heard as a gap\n"
    "  --no-mic        do not open the microphone (M still fakes one)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --timing-oc     Timing OC: no GX FIFO, untimed geometry (faster, less accurate; DraStic's model).\n"
    "                  emu.timing_oc in the config\n"
    "  --cpu-oc        CPU tuning, overclock: recompiled data accesses priced as main RAM, geometry on its\n                  own thread with every polygon priced as drawn (less accurate); emu.cpu_tuning = overclock\n"
    "  --cpu-uc        CPU tuning, underclock: for the harder to run games and/or the lowest end devices.\n                  The game's CPUs run slower than a console's, so there is less to emulate a frame\n                  (less accurate; a game can miss VBlanks); emu.cpu_tuning = underclock\n"
    "  --fast-load     cart DMA reads the card without its clock (may affect accuracy); emu.fast_load\n"
    "  --aa / --no-aa  3D anti-aliasing on (hardware behaviour) or off; video.aa, off by default\n"
    "  --lockstep      128-cycle CPU interleave (melonDS lockstep) instead of event-bound; --quantum N for any value\n"
    "  --frameskip N   skip drawing up to N frames in N+1 (0 = off); emu.frameskip. Skipping runs\n"
    "                  in whole display periods, so on a game that drives its screens on\n"
    "                  alternate frames the limit counts pairs (DS_DEBUG_SKIP=1 shows the period)\n"
    "  --frameskip-mode M  adaptive (default; skip only while the emulator is behind, up to N)\n"
    "                  | fixed (always skip N of every N+1)\n"
    "  --no-frameskip-capture  do not skip frames that display-capture. Exact, but a game that\n"
    "                  captures every frame -- Pokemon B/W, Golden Sun -- then skips nothing,\n"
    "                  so frameskip does nothing at all on it. On by default (the captured\n"
    "                  VRAM holds the last drawn frame); emu.frameskip_capture\n"
    "  --limiter HZ    the rate the emulator is held to: auto (the console's own 59.8261 Hz, the\n"
    "                  default and the speed a game was written for), 30, 60 (what a player means\n"
    "                  by 60 fps: 0.29 % fast, and in step with a 60 Hz panel), 120, 144, 240, or\n"
    "                  off. emu.limiter\n"
    "  --speed N       run at N percent of that rate (25..400); emu.speed\n"
    "  --pacing M      how the wait for the next frame is spent: auto (default) | sleep | busy.\n"
    "                  A frequency governor that decides by polling how busy the last few\n"
    "                  milliseconds looked reads that sleep as an idle machine and clocks down\n"
    "                  under the emulator (ondemand on ROCKNIX: 4-6x the missed frames and ~20x\n"
    "                  the audio gaps). busy holds the core to the deadline instead, at the price\n"
    "                  of a core that never idles; auto does that only where such a governor is in\n"
    "                  charge. Nothing here changes a system setting. emu.pacing\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --stats-from N  leave the first N frames out of the frame statistics (DS_FRAME_STATS)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n"
    "  --rtc-host      run the clock from this machine even under --replay (INEXACT: a game\n"
    "                  that reads the date no longer replays the same, but the firmware's own\n"
    "                  menu needs a real clock to appear at all)\n"
    "  --netplay       local wireless with whoever is on the LAN: join a session heard within\n"
    "                  2.5 s, else host one (melonDS's LAN protocol; a melonDS can be the other end).\n"
    "                  The menu's NETWORK FEATURES row and net.mode do the same without a flag.\n"
    "                  Any local wireless forces --cpu-oc, --timing-oc and --fast-load off: the\n"
    "                  two consoles in a session have to keep the same time as each other\n"
    "  --lan-host NAME host a local-wireless session as NAME; --lan-join ADDR joins the one at ADDR;\n"
    "                  --lan-name NAME is our player name (default: the console's nickname)\n"
    "  --internet      the game reaches the real network through the emulated access point, over a\n"
    "                  user-mode TCP/IP stack (no privileges, works on wlan0). Not with local wireless\n"
    "  --dns WHERE     where the game looks up its servers: wiimmfi (the default: Nintendo's own\n"
    "                  servers were switched off in 2014), host (this machine's resolver), or an address\n"
    "  --load-state F  start from a save state instead of booting the game\n"
    "  --autosave-png F  with emu.autosave, write a PNG of both screens to F beside the auto state\n"
#if DSPERATE_CHEEVOS
    "  --cheevos         RetroAchievements for this run (Casual mode; needs a sign-in)\n"
    "  --no-cheevos      off for this run, whatever the config says\n"
    "  --cheevos-token F  sign in with the token in F instead of a stored sign-in (the format a\n"
    "                  CFW's PPSSPP helper writes: the token alone, with the account name coming\n"
    "                  from cheevos.username or --cheevos-user). Implies --cheevos\n"
    "  --cheevos-user U  the account that token signs in as, instead of cheevos.username\n"
#endif
    "  --autoload      start from the auto slot (emu.autosave's state) when there is one\n"
    "  --no-autoload   ignore it for this run, whatever emu.autoload says\n"
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

// The four-letter game code from a plain .nds header (offset 0x0C), for the
// per-game config file. Read straight off the file because the config layering
// has to happen before the cart is loaded -- the display, audio and input are
// all configured from `cfg` well before that, so a per-game [video] or [keys]
// merged at cart-load time would arrive too late to be read. A zip is not
// peeked (its entry is only known once unpacked); those fall back to the
// filename file early and pick up the code file at the later merge.
bool peek_game_code(const std::string& rom, char out[4]) {
  if (rom_ext(rom) != ".nds") return false;
  FILE* f = std::fopen(rom.c_str(), "rb");
  if (!f) return false;
  char code[4] = {};
  const bool ok = std::fseek(f, 0x0C, SEEK_SET) == 0 && std::fread(code, 1, 4, f) == 4;
  std::fclose(f);
  // A header's code is four printable characters; anything else is not one.
  for (int i = 0; ok && i < 4; ++i)
    if (code[i] < 0x20 || code[i] >= 0x7f) return false;
  if (!ok) return false;
  std::memcpy(out, code, 4);
  return true;
}

// Battery save: next to the ROM, or under [paths] saves.
std::string save_path(const std::string& rom, const std::string& dir) {
  return dir.empty() ? rom_stem(rom) + ".sav" : dir + "/" + base_name(rom_stem(rom)) + ".sav";
}

void load_save(NDS& nds, const std::string& path) {
  if (!nds.cart) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (!nds.cart->save_type_listed()) VLOG("save: game code not in the save list; the chip is detected on first use\n");
    return;
  }
  std::vector<u8> data;
  u8 buf[65536];
  for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
  std::fclose(f);
  const ds::cart::Cart::SaveLoad r = nds.cart->load_save(data.data(), data.size());
  if (r.fitted) { VLOG("save: loaded %zu bytes from %s\n", data.size(), path.c_str()); return; }
  // The first write replaces the file with one of the chip's size, which
  // would cut off (or pad) the player's original: keep it beside the save.
  const std::string bak = path + ".bak";
  if (r.chip_bytes) std::fprintf(stderr, "save: %s is %zu bytes but the game's save chip holds %u; the original is kept as %s\n", path.c_str(), data.size(), r.chip_bytes, bak.c_str());
  else std::fprintf(stderr, "save: %s is %zu bytes, a size no save chip has; ignored, the original is kept as %s\n", path.c_str(), data.size(), bak.c_str());
  if (FILE* e = std::fopen(bak.c_str(), "rb")) { std::fclose(e); return; }   // an earlier original wins
  if (FILE* o = std::fopen(bak.c_str(), "wb")) {
    if (std::fwrite(data.data(), 1, data.size(), o) != data.size()) std::fprintf(stderr, "save: cannot write %s\n", bak.c_str());
    std::fclose(o);
  }
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

// emu.cpu_tuning (once emu.cpu_oc; Config::load reads the old key and value):
// false | underclock | overclock, "true" still meaning overclock. 0/1/2, the
// order of jit::CpuOc; anything unreadable is off. Also read as "is it on"
// for the two boolean speed knobs.
int cpu_oc_mode(const std::string& v) {
  if (v == "underclock") return 2;
  return (v == "overclock" || v == "1" || v == "true" || v == "yes" || v == "on") ? 1 : 0;
}

// The CPU tuning tier to run, given the one set (cpu_oc_mode's 0/1/2). On a
// DSi NAND boot:
// - the underclock waits until the DSi Menu has jumped to the title it
//   launched (NDS::dsi_title_running): the Menu's hand-off is an IPC race
//   the underclocked ARM9 loses, and both CPUs then wait for good;
// - PictoChat (HNE?) and DS Download Play (HND?) run with neither tier:
//   PictoChat's ARM9 builds its heap in the RAM its ARM7 is still clearing
//   and loses that race under both, and Download Play is local wireless,
//   which a session runs untuned anyway. A program Download Play boots keeps
//   its setting (the header snapshot still says HND).
int cpu_tuning_for(const NDS& nds, int mode) {
  if (mode == 0 || !nds.dsi || !nds.dsi_nand_boot) return mode;
  if (!nds.dsi_title_running) return mode == 2 ? 0 : mode;
  const char* c = nds.dsi_title_code;
  if (c[0] == 'H' && c[1] == 'N' && (c[2] == 'E' || c[2] == 'D')) return 0;
  return mode;
}

// Autoload: the auto slot as the starting point, when emu.autoload asks for
// it and the file is there. The state is left in place -- a SIGKILL never
// gets to write one, so the last clean exit's state stays resumable, and a
// clean exit overwrites it anyway.
std::string autoload_path(NDS& nds, const std::string& dir, bool enabled) {
  // Never for a cart-less firmware boot: auto_state_path() keys on the "NONE"
  // code there, and would pick up whatever an earlier cart-less session left.
  if (!enabled || !nds.cart) return {};
  const std::string path = auto_state_path(nds, dir);
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fclose(f);
  return path;
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

// Achievement progress, riding after the layout for the same reasons and with
// the same rules: the frontend's chunk, absent from a headless or older file,
// no format version bump.
//
// This matters more in Casual mode than it might look. Save states are allowed
// here -- that is the whole point of the mode -- so without this, loading a
// state leaves the achievement runtime describing a world that no longer
// exists: hit counts part-way towards something the player has just rewound
// past, triggers primed by events that have been undone. drastic-nano could
// skip it because its hardcore mode forbids loading states at all; we cannot.
//
// The hook is a file-scope pointer rather than an argument because every state
// in the process belongs to the one session, and there are six call sites that
// save or load. A defaulted parameter would let one of them quietly forget --
// the autosave path especially -- and the symptom would be achievement
// progress that is wrong only sometimes.
// Saving only. Reading needs no hook at all -- the chunk goes into
// g_cheevos_pending below and is applied later, for the ordering reason
// explained there.
struct CheevosStateHook {
  virtual ~CheevosStateHook() = default;
  // False when there is nothing to carry, and the chunk is then not written.
  virtual bool save(u32& game_id, std::vector<u8>& blob) = 0;
};
CheevosStateHook* g_cheevos_state = nullptr;

// The chunk a state carried, held until there is a set to put it into.
//
// This is not an optimisation, it is the only order that works. A state named
// on the command line -- or resumed from the auto slot -- is loaded at startup,
// long before the session has signed in and fetched the game's achievements;
// the device log makes it plain:
//
//     state: loaded ... (frame 400)
//     cheevos: session up ...
//     cheevos: signed in as Noxwell
//
// So the chunk is remembered here and applied when the set arrives. The same
// path covers an in-session load whose set is still being fetched, and means
// there is one rule rather than two.
struct PendingCheevosState {
  bool waiting = false;      // a state was loaded and has not been accounted for
  u32 game_id = 0;
  std::vector<u8> blob;      // empty: the state carried no progress, so reset
} g_cheevos_pending;

void write_cheevos_chunk(ds::state::Writer& w) {
  if (!g_cheevos_state) return;
  u32 game_id = 0;
  std::vector<u8> blob;
  if (!g_cheevos_state->save(game_id, blob) || blob.empty()) return;
  w.begin("CHVO");
  w.put(game_id);
  w.vec(blob);
  w.end();
}

void read_cheevos_chunk(ds::state::Reader& r) {
  // Deliberately no check for the hook: at startup the state is read before
  // the session exists, and the whole point is that the chunk survives that.
  // No chunk -- an older state, a headless one, or one saved with achievements
  // off. The runtime still has to be put back to the start, or it would go on
  // counting against a machine that has just jumped somewhere else.
  if (r.at_end() || !r.begin("CHVO")) { g_cheevos_pending = {true, 0, {}}; return; }
  u32 game_id = 0;
  std::vector<u8> blob;
  r.fields(game_id);
  r.vec(blob);
  r.end();
  if (!r.ok() || blob.empty()) { g_cheevos_pending = {true, 0, {}}; return; }
  g_cheevos_pending = {true, game_id, std::move(blob)};
}


bool save_state_file(NDS& nds, const std::string& path, const ds::sdl::Display::Layout& layout) {
  ds::state::Writer w; std::string err;
  if (!nds.save_state(w, err)) { std::fprintf(stderr, "state: cannot save: %s\n", err.c_str()); return false; }
  write_layout_chunk(w, layout);
  write_cheevos_chunk(w);
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
// Set when load_state_file() was given a real state and the core refused it
// -- a BIOS mismatch, another ROM, an older format. Not set for an empty or
// unreadable slot, which is nothing to report. The menu shows it on the slot
// row (Menu::set_slot_notice); the reason has already gone to the log.
std::string g_state_refused;

bool load_state_file(NDS& nds, const std::string& path, ds::sdl::Display::Layout& layout, bool& layout_loaded) {
  layout_loaded = false;
  std::vector<u8> bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n > 0) { bytes.resize(static_cast<size_t>(n)); if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
    std::fclose(f);
  }
  if (bytes.empty()) { std::fprintf(stderr, "state: no state in slot (%s)\n", path.c_str()); return false; }
  g_state_refused.clear();
  ds::state::Reader r(bytes.data(), bytes.size());
  std::string err;
  if (!nds.load_state(r, err)) {
    std::fprintf(stderr, "state: cannot load %s: %s\n", path.c_str(), err.c_str());
    g_state_refused = "REJECTED";
    return false;
  }
  layout_loaded = read_layout_chunk(r, layout);
  read_cheevos_chunk(r);
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
// Where NAND title shortcuts go (emu.dsi_nand_shortcuts): paths.dsi_shortcuts,
// or the games folder when that is unset -- a frontend that keeps DSiWare in a
// system folder of its own points the first at it.
std::string shortcuts_dir(const ds::sdl::Config& cfg) {
  const std::string d = cfg.str("paths.dsi_shortcuts");
  return d.empty() ? cfg.str("paths.games") : d;
}

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
    if (ext != "nds" && ext != "zip" && ext != "dsi" && ext != "cia") continue;
    // DSiWare runs on the DSi machine (the launcher hand-off, see the Launch
    // result below); it is marked so it is not mistaken for a card game.
    const std::string path = dir + "/" + name;
    // A NAND title shortcut (io/dsi_nand_launch.h): the title installed on
    // paths.dsi_nand, started without the DSi Menu.
    if (ds::io::is_shortcut_name(name)) {
      games.push_back({"[DSi] " + name.substr(0, name.size() - std::strlen(ds::io::kShortcutSuffix)), path});
      continue;
    }
    const bool dsi = ds::io::file_is_dsiware(path);
    if (!dsi && ext != "nds" && ext != "zip") continue;
    games.push_back({dsi ? "[DSi] " + rom_stem(name) : rom_stem(name), path});
  }
  closedir(d);
  // By name, the label aside, so a DSiWare title sits among the rest.
  auto key = [](const std::string& t) { return t.compare(0, 6, "[DSi] ") == 0 ? t.substr(6) : t; };
  std::sort(games.begin(), games.end(), [&](const ds::sdl::Menu::GameEntry& a, const ds::sdl::Menu::GameEntry& b) {
    return key(a.title) < key(b.title);
  });
  return games;
}

// The loader card's list: the games folder, and the shortcut folder's titles
// too when that is somewhere else, so the shortcuts are on the list either way.
std::vector<ds::sdl::Menu::GameEntry> enumerate_library(const ds::sdl::Config& cfg) {
  std::vector<ds::sdl::Menu::GameEntry> games = enumerate_games(cfg.str("paths.games"));
  const std::string sc = cfg.str("paths.dsi_shortcuts");
  if (sc.empty() || sc == cfg.str("paths.games")) return games;
  for (ds::sdl::Menu::GameEntry& g : enumerate_games(sc))
    if (ds::io::is_shortcut_name(g.path)) games.push_back(std::move(g));
  auto key = [](const std::string& t) { return t.compare(0, 6, "[DSi] ") == 0 ? t.substr(6) : t; };
  std::stable_sort(games.begin(), games.end(), [&](const ds::sdl::Menu::GameEntry& a, const ds::sdl::Menu::GameEntry& b) {
    return key(a.title) < key(b.title);
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
    // The header comes from the loaded cart, not from the file at rom_path:
    // for a zipped game that file starts with the archive's own header and
    // every lookup would miss.
    u8 header[512] = {};
    if (nds.cart) nds.cart->rom_read(0, header, sizeof header);
    if (nds.cart && ds::cheat::load_for_header(db, header, cheats, err)) {
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

// A canvas over a DS-sized scratch buffer: the fallback for the tiers with no
// panel-resolution surface the CPU may write. At 256x192 the menu's own scale
// works out to 2, which is the size it drew at when it lived in DS space, so
// this path puts down exactly the pixels it always did.
ds::sdl::Canvas ds_canvas(u32* px) {
  return ds::sdl::Canvas{px, ds::SCREEN_W, static_cast<int>(ds::SCREEN_W), static_cast<int>(ds::SCREEN_H)};
}
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

// A small white label (3x5 font: digits, capitals and spaces; anything else
// draws as a space) on a black box, anchored to a corner of the canvas.
// `right` anchors to the right edge instead of the left, which is how the two
// callers -- the state slot field top-left, the FPS counter top-right -- stay
// clear of each other.
//
// There is no `bottom` any more. It existed because the overlays were drawn
// before the PiP inset was blitted, so one sharing the inset's corner was
// buried and had to dodge to the opposite edge. Drawn on the canvas after
// Display::finish_views(), nothing can be drawn over them.
ds::sdl::Rect draw_label(const ds::sdl::Canvas& d, const char* text, bool right) {
  static const u8 digits[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}};
  static const u8 letters[26][5] = {
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3},
    {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {6,5,5,5,5},
    {2,5,5,5,2}, {6,5,6,4,4}, {2,5,5,6,3}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2}, {5,5,5,5,7},
    {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2}, {7,1,2,4,7}};
  static const u8 blank[5] = {0,0,0,0,0};
  auto glyph = [&](char c) -> const u8* {
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    return blank;
  };
  auto fill = [&](int x, int y, u32 colour) {
    if (x < 0 || x >= d.w || y < 0 || y >= d.h) return;
    d.px[static_cast<size_t>(y) * d.pitch + static_cast<size_t>(x)] = colour;
  };
  int n = 0; while (text[n]) ++n;
  if (n == 0) return {};
  // The same measure the menu uses, so the two agree on how big a pixel is.
  // At 256x192 -- the DS-space fallback -- it is 2, the scale this drew at
  // when it lived in DS pixels, and the box comes out where it always did.
  const int S = ds::sdl::ui_scale(d);
  const int pad = S, margin = 2 * S;
  const int w = n * 3 * S + (n - 1) * S + 2 * pad;   // glyphs, one S-wide gap between each, a border
  const int h = 5 * S + 2 * pad;
  const int X = right ? d.w - margin - w : margin;
  const int Y = margin;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) fill(X + x, Y + y, 0xFF000000);
  for (int g = 0; g < n; ++g) {
    const u8* f = glyph(text[g]);
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
      if ((f[r] >> (2 - c)) & 1)
        for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x)
          fill(X + pad + g * 4 * S + c * S + x, Y + pad + r * S + y, 0xFFFFFFFF);
  }
  return ds::sdl::Rect{X, Y, w, h};
}

// The state slot field: the slot's digit after a slot hotkey, or what just
// happened to the slot ("STATE 3 SAVED") after a state hotkey.
constexpr int SLOT_OSD_FRAMES = 90;

// The screenshot flash: white blended over a whole screen at `alpha`, the
// frame after the picture is taken and fading over FLASH_FRAMES. It is drawn
// where the other overlays are -- into the presented buffer, never into the
// GPU's framebuffers the screenshot reads -- so it is never in the picture.
constexpr int FLASH_FRAMES = 12;
void draw_flash(const ds::sdl::Canvas& d, u32 alpha) {
  if (d.w <= 0 || d.h <= 0) return;
  // The whole canvas, margins included: the flash says the picture was taken,
  // and on the canvas that is the whole window rather than one screen's view.
  std::vector<u32> white(static_cast<size_t>(d.w), 0xFFFFFFFFu);
  for (int y = 0; y < d.h; ++y)
    ds::sdl::Display::blend_row(d.px + static_cast<size_t>(y) * d.pitch, white.data(), static_cast<size_t>(d.w), alpha);
}

// A launcher's SIGTERM (or Ctrl-C) must still flush the battery save.
volatile std::sig_atomic_t g_signalled = 0;
void on_signal(int) { g_signalled = 1; }


// Everything [video] settles before a window exists, in one place, so that the
// same values can be re-read and re-applied when a setting is changed from the
// pause menu rather than only at boot. parse_video() is pure: it reads the
// config and never touches SDL. open_displays() is the other half, the set_*
// calls that have to happen before Display::open because the scanline scaler
// builds its tables there.
//
// The tier fields (disp, fbdev, and the dual-window panel order) are decided
// once at boot and are not re-derived: choosing them forces SDL's video driver,
// which is settled before SDL_Init and cannot be changed with a window open.
struct VideoSetup {
  int    scale = 2;
  bool   fullscreen = false, linear = false, vsync = true, dual_window = false;
  ds::sdl::Display::IntScale int_scale = ds::sdl::Display::IntScale::Off;
  double grid_s = 0.0;      // 0..1 as configured
  u32    grid = 256;        // brightness kept on a seam, 0..256 (256 = off)
  u8     seam_blend = 0;    // 0 dark, 1 blend, 2 blend in linear light
  u8     chunky = 0;
  int    chunky_cell = -1;
  u32    chunky_thresh = 180 * 256;
  ds::sdl::Display::Layout layout;
  std::vector<ds::sdl::Display::Mode> layout_cycle;
  // Boot-only, see above.
  bool   use_disp = false, use_fbdev = false;
  int    bottom_display = 1;
};

constexpr const char* kDefaultLayoutCycle = "vertical,horizontal,single,pip,dominant_v,dominant_h";

// "vertical, pip" -> the modes, in the file's order. Duplicates are kept as
// written; an unknown name is reported and fails the parse. `out` is what the
// file said, possibly empty -- the caller decides what an empty ring means.
bool parse_layout_cycle(const std::string& cyc, std::vector<ds::sdl::Display::Mode>& out) {
  using Disp = ds::sdl::Display;
  out.clear();
  for (size_t at = 0; at <= cyc.size();) {
    size_t end = cyc.find(',', at); if (end == std::string::npos) end = cyc.size();
    std::string name = cyc.substr(at, end - at);
    name.erase(0, name.find_first_not_of(' ')); name.erase(name.find_last_not_of(' ') + 1);
    Disp::Mode m;
    if (!name.empty() && !Disp::parse_mode(name, m)) { std::fprintf(stderr, "unknown layout %s in layout_cycle\n", name.c_str()); return false; }
    if (!name.empty()) out.push_back(m);
    at = end + 1;
  }
  return true;
}

std::string layout_cycle_string(const std::vector<ds::sdl::Display::Mode>& cycle) {
  std::string s;
  for (ds::sdl::Display::Mode m : cycle) { if (!s.empty()) s += ','; s += ds::sdl::Display::mode_name(m); }
  return s;
}

bool parse_video(const ds::sdl::Config& cfg, VideoSetup& vs) {
  using Disp = ds::sdl::Display;
  vs.scale = cfg.num("video.scale", 2);
  if (vs.scale < 1) vs.scale = 1;
  vs.fullscreen = cfg.flag("video.fullscreen", false);
  vs.linear = cfg.flag("video.linear", false);
  vs.vsync = cfg.flag("video.vsync", true);
  vs.dual_window = cfg.flag("video.dual_window", false);
  if (!Disp::parse_int_scale(cfg.str("video.integer_scale", "off"), vs.int_scale)) { std::fprintf(stderr, "integer_scale must be off, under or over\n"); return false; }
  // Grid strength -> brightness kept on the seams, 0..256 (256 = off).
  vs.grid_s = std::min(1.0, std::max(0.0, cfg.real("video.lcd_grid", 0.0)));
  vs.grid = static_cast<u32>(std::lround((1.0 - vs.grid_s) * 256.0));
  vs.seam_blend = 0;
  {
    const std::string sm = cfg.str("video.seam", "dark");
    if (sm == "blend") vs.seam_blend = 1; else if (sm == "blend_linear") vs.seam_blend = 2;
    else if (sm != "dark") { std::fprintf(stderr, "unknown seam %s (dark | blend | blend_linear)\n", sm.c_str()); return false; }
    if (vs.linear && (vs.grid_s > 0.0 || vs.seam_blend || cfg.str("video.chunky", "false") != "false"))
      std::fprintf(stderr, "video.linear takes precedence over lcd_grid, seam and chunky\n");
  }
  {
    const std::string c = cfg.str("video.chunky_cell", "auto");
    if (c == "auto") vs.chunky_cell = -1; else if (c == "pair" || c == "2x") vs.chunky_cell = 0;
    else { vs.chunky_cell = std::atoi(c.c_str()); if (vs.chunky_cell < 2 || vs.chunky_cell > 64) { std::fprintf(stderr, "chunky_cell %s: auto | pair | 2..64\n", c.c_str()); return false; } }
  }
  vs.chunky_thresh = static_cast<u32>(std::min(255, std::max(0, cfg.num("video.chunky_threshold", 180)))) * 256;
  vs.chunky = 0;
  {
    const std::string c = cfg.str("video.chunky", "false");
    if (c == "tl") vs.chunky = 1; else if (c == "mean" || c == "true" || c == "1" || c == "yes" || c == "on") vs.chunky = 2;
    else if (c == "min") vs.chunky = 4; else if (c == "max") vs.chunky = 5; else if (c == "mode") vs.chunky = 3;
    else if (c == "extreme") vs.chunky = 6;
    else if (!(c == "false" || c == "0" || c == "no" || c == "off" || c.empty())) { std::fprintf(stderr, "unknown chunky %s (mean | extreme | mode | tl | min | max | false)\n", c.c_str()); return false; }
  }
  vs.layout = Disp::Layout{};
  vs.layout_cycle.clear();
  {
    const std::string l = cfg.str("video.layout", "vertical"), sc = cfg.str("video.screen", "top"), co = cfg.str("video.pip_corner", "br");
    if (!Disp::parse_mode(l, vs.layout.mode)) { std::fprintf(stderr, "unknown layout %s\n", l.c_str()); return false; }
    if (sc == "top") vs.layout.primary = 0; else if (sc == "bottom") vs.layout.primary = 1;
    else { std::fprintf(stderr, "unknown screen %s (top | bottom)\n", sc.c_str()); return false; }
    if (!Disp::parse_corner(co, vs.layout.corner)) { std::fprintf(stderr, "unknown pip_corner %s (tl | tr | bl | br)\n", co.c_str()); return false; }
    // The ring layout_next/prev step through; a mode outside it joins at its start.
    if (!parse_layout_cycle(cfg.str("video.layout_cycle", kDefaultLayoutCycle), vs.layout_cycle)) return false;
    if (vs.layout_cycle.empty()) vs.layout_cycle.push_back(vs.layout.mode);
    vs.layout.pip = std::clamp(cfg.real("video.pip_scale", 1.0 / 3.0), 0.1, 0.9);
    vs.layout.pip_alpha = std::clamp(cfg.real("video.pip_alpha", 1.0), 0.0, 1.0);
    const std::string dr = cfg.str("video.dominant_ratio", "auto");
    vs.layout.dominant_auto = dr == "auto";
    if (!vs.layout.dominant_auto) {
      char* end = nullptr; const double v = std::strtod(dr.c_str(), &end);
      if (end == dr.c_str() || *end) { std::fprintf(stderr, "dominant_ratio must be a number or auto\n"); return false; }
      vs.layout.dominant = std::clamp(v, 0.1, 0.99);
    }
    vs.layout.dominant_min = std::clamp(cfg.real("video.dominant_threshold", 0.25), 0.1, 0.99);
  }
  return true;
}

// The set_* calls and the open itself. Split from parse_video so a settings
// change can close the windows and come back through here with new values.
bool open_displays(const VideoSetup& vs, ds::sdl::Display& display, ds::sdl::Display& display2) {
  if (vs.dual_window) {
    display.set_chunky(vs.chunky != 0, vs.chunky_cell); display2.set_chunky(vs.chunky != 0, vs.chunky_cell);
    display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s); display2.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
    display.set_integer_scale(vs.int_scale); display2.set_integer_scale(vs.int_scale);
    if (!display.open("DSperate", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 0, 1 - vs.bottom_display) ||
        !display2.open("DSperate (Bottom)", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout, 1, vs.bottom_display)) return false;
    if (display.scaling() != display2.scaling()) { std::fprintf(stderr, "dual-window: mixed display modes\n"); return false; }
    return true;
  }
  display.set_chunky(vs.chunky != 0, vs.chunky_cell);
  display.set_disp(vs.use_disp);
  display.set_integer_scale(vs.int_scale);
  display.set_grid_strength(vs.linear ? 0.0 : vs.grid_s);
  if (!vs.linear) display.set_disp_grid(static_cast<u8>(((256 - vs.grid) * 255) / 256));
  display.set_fbdev(vs.use_fbdev);
  return display.open("DSperate", vs.scale, vs.fullscreen, vs.linear, vs.vsync, vs.layout);
}

// The Controls page's rows that are neither a DS button nor a hotkey: the
// modifier a pad chord is built on, and the pen's tap button and stick. The
// modifier exists in both columns; the pen is the pad's, so its two rows are
// only offered there.
struct Extra { const char* key_keys; const char* key_pad; const char* label; };
constexpr Extra kExtras[] = {
  {"hotkeys.modifier", "padhotkeys.modifier", "MODIFIER"},
  {nullptr,            "pad.stylus_button",   "STYLUS TAP"},
  {nullptr,            "pad.stylus_button.alt", "STYLUS TAP (2)"},
  {nullptr,            "pad.stylus_axis",     "STYLUS STICK"},
  {nullptr,            "pad.stylus_dpad",     "STYLUS DPAD"},
  {nullptr,            "pad.stick_dpad",      "STICK DPAD"},
  {nullptr,            "pad.stick_face",      "STICK ABXY"},
  // Axis remapping: each row is set by pushing the control the way that axis
  // reads positive -- a stick right for X, down for Y, a trigger pressed --
  // and stores the physical axis that went, inverted if it went negative.
  {nullptr,            "pad.axis_leftx",      "L STICK RIGHT"},
  {nullptr,            "pad.axis_lefty",      "L STICK DOWN"},
  {nullptr,            "pad.axis_rightx",     "R STICK RIGHT"},
  {nullptr,            "pad.axis_righty",     "R STICK DOWN"},
  {nullptr,            "pad.axis_lefttrigger", "L2 AXIS"},
  {nullptr,            "pad.axis_righttrigger", "R2 AXIS"},
};
// The rows that hold a stick rather than a control: a captured axis is
// stored as the stick it belongs to, and the value reads as one.
bool extra_is_stick(const char* key) { return std::strcmp(key, "pad.stylus_axis") == 0 || std::strcmp(key, "pad.stick_dpad") == 0 || std::strcmp(key, "pad.stick_face") == 0; }
bool extra_is_axis_remap(const char* key) { return std::strncmp(key, "pad.axis_", 9) == 0; }
// "-righty" as the page reads it: which physical control, and whether it runs backwards.
std::string axis_remap_label(const std::string& v0) {
  if (v0 == "none") return "NONE";
  const bool inv = !v0.empty() && v0[0] == '-';
  const std::string v = !v0.empty() && (v0[0] == '-' || v0[0] == '+') ? v0.substr(1) : v0;
  std::string n = v == "leftx" ? "L STICK X" : v == "lefty" ? "L STICK Y" : v == "rightx" ? "R STICK X" : v == "righty" ? "R STICK Y"
                : v == "lefttrigger" ? "L2" : v == "righttrigger" ? "R2" : v;
  for (char& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return inv ? n + " INV" : n;
}
bool extra_in_column(const Extra& e, bool pad) { return pad || e.key_keys; }
int extra_count(bool pad) {
  int n = 0;
  for (const Extra& e : kExtras) n += extra_in_column(e, pad) ? 1 : 0;
  return n;
}
const Extra& extra_at(bool pad, int j) {
  for (const Extra& e : kExtras)
    if (extra_in_column(e, pad) && j-- == 0) return e;
  return kExtras[0];   // unreachable: callers bound j by extra_count()
}
const char* extra_key(const Extra& e, bool pad) { return pad ? e.key_pad : e.key_keys; }

// What the emulator starts from, so the page's "defaults" is the same answer
// Input::configure() would give. One lookup for showing a row and for
// resetting it: two would drift the moment a default changed.
const char* extra_default(const char* key, bool pad, const ds::sdl::Config& cfg) {
  if (std::strcmp(key, "pad.stylus_button") == 0) return ds::sdl::Input::stylus_button_default();
  if (std::strcmp(key, "pad.stylus_button.alt") == 0) return "none";   // a second binding has no default (input.h)
  if (std::strcmp(key, "pad.stylus_axis") == 0)   return ds::sdl::Input::stylus_axis_default(cfg);
  if (std::strcmp(key, "pad.stylus_dpad") == 0)   return ds::sdl::Input::stylus_dpad_default();
  if (std::strcmp(key, "pad.stick_face") == 0)    return ds::sdl::Input::stick_face_default();
  if (std::strcmp(key, "pad.stick_dpad") == 0)    return ds::sdl::Input::stick_dpad_default();
  if (extra_is_axis_remap(key))
    for (int i = 0; i < ds::sdl::Input::axis_remap_count(); ++i)
      if (ds::sdl::Input::axis_remap_key(i) == key) {
        static std::string defaults[SDL_CONTROLLER_AXIS_MAX];   // stable storage for the returned pointer
        defaults[i] = ds::sdl::Input::axis_remap_default(i);
        return defaults[i].c_str();
      }
  return ds::sdl::Input::mod_default(pad);
}

} // namespace

int main(int argc, char** argv) {
  const char* rom = nullptr;
  const char* config_arg = nullptr;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr, *save_arg = nullptr, *load_state = nullptr;
  bool clear_cache = false;
  bool rtc_host = false;              // --rtc-host: a real clock even under a replay (the firmware menu needs one)
  const char* lan_host = nullptr; const char* lan_join = nullptr; const char* lan_name = "DSperate"; bool netplay = false;
  bool lan_guest = false;             // net.mode = guest: join a session heard on the LAN, never host one
  bool lan_name_set = false;          // --lan-name was given, so the nickname must not override it
  bool internet = false;              // --internet / net.mode = internet: the emulated AP reaches the real network
  const char* dns_arg = nullptr;      // --dns: host, wiimmfi, or an address
  long stats_from = 0;   // frames run but left out of the timing statistics
  // DSi mode (docs/dsiware-scoping.md): the plumbing only, from the command
  // line. Config keys and the menu come once the rest of the DSi path is in.
  bool dsi_mode = false;
  const char* bios9i = nullptr; const char* bios7i = nullptr; const char* dsi_nand = nullptr;
  const char* dsi_title = nullptr; const char* dsi_tmd = nullptr; bool dsi_offline = false, dsi_hide_installed = false, dsi_menu = false;
  bool dsi_hle = false;          // --dsi-mode with a title and no NAND: the launcher hand-off, BIOS pair only
  const char* dsi_font = nullptr;
  const char* dsi_sd = nullptr;
  u32 dsi_title_lo = 0;   // the title --dsi-mode was given, once it is on the NAND: what the autoload starts
  std::string dsi_title_cheevos_hash;   // its RetroAchievements identity: it runs from the NAND, with no cart to hash later

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
    else if (flag("--dsi-mode")) dsi_mode = true;
    else if (arg("--bios9i")) bios9i = argv[++i];
    else if (arg("--bios7i")) bios7i = argv[++i];
    else if (arg("--dsi-nand")) dsi_nand = argv[++i];
    else if (arg("--dsi-tmd")) dsi_tmd = argv[++i];
    else if (arg("--dsi-font")) dsi_font = argv[++i];
    else if (arg("--dsi-sd")) dsi_sd = argv[++i];
    else if (flag("--dsi-offline")) dsi_offline = true;
    else if (flag("--dsi-menu")) dsi_menu = true;
    else if (flag("--dsi-hide-installed")) dsi_hide_installed = true;
    else if (arg("--config")) config_arg = argv[++i];
    else if (arg("--write-config")) { ds::sdl::Config::write_default(argv[++i], true); return 0; }
    else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--pacing")) cli.set("emu.pacing", argv[++i]);
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--screen")) cli.set("video.screen", argv[++i]);
    else if (arg("--pip-alpha")) cli.set("video.pip_alpha", argv[++i]);
    else if (arg("--dominant-ratio")) cli.set("video.dominant_ratio", argv[++i]);
    else if (arg("--dominant-threshold")) cli.set("video.dominant_threshold", argv[++i]);
    else if (flag("--integer-scale")) cli.set("video.integer_scale", optional("under"));
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (flag("--rtc-host")) rtc_host = true;
    // Local wireless over the LAN for this run (docs/wifi-scoping.md): host a
    // session, or join the one at ADDR. The menu's NETWORK FEATURES row
    // (net.mode) does the same without an address; a flag wins over it.
    else if (arg("--lan-host")) lan_host = argv[++i];
    else if (arg("--lan-join")) lan_join = argv[++i];
    else if (arg("--lan-name")) { lan_name = argv[++i]; lan_name_set = true; }
    else if (flag("--netplay")) netplay = true;   // join a session heard on the LAN within 2.5 s, else host one
    // The internet through the emulated access point (docs/wifi-scoping.md).
    // Exclusive with local wireless: one radio, one use of it per session.
    else if (flag("--internet")) internet = true;
    else if (arg("--dns")) dns_arg = argv[++i];
    else if (flag("--clear-cache")) clear_cache = true;
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (arg("--load-state")) load_state = argv[++i];
    else if (arg("--autosave-png")) cli.set("emu.autosave_png", argv[++i]);
#if DSPERATE_CHEEVOS
    // Turning RetroAchievements on for one run, without editing the config.
    else if (flag("--cheevos")) cli.set("cheevos.enabled", "true");
    else if (flag("--no-cheevos")) cli.set("cheevos.enabled", "false");
    // A token file somebody else wrote (PPSSPP's shape: the token on its own).
    // Wanting it is wanting RetroAchievements, so it turns them on; a later
    // --no-cheevos still wins, because these are applied in order.
    else if (arg("--cheevos-token")) { cli.set("cheevos.token_file", argv[++i]); cli.set("cheevos.enabled", "true"); }
    // The account that token belongs to, for a launcher that would rather pass
    // both than keep a name in the config.
    else if (arg("--cheevos-user")) cli.set("cheevos.username", argv[++i]);
#endif
    else if (flag("--autoload")) cli.set("emu.autoload", "true");
    else if (flag("--no-autoload")) cli.set("emu.autoload", "false");
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
    else if (arg("--audio-buffer")) cli.set("audio.buffer_size", argv[++i]);
    else if (arg("--speed")) cli.set("emu.speed", argv[++i]);
    else if (arg("--limiter")) cli.set("emu.limiter", argv[++i]);
    else if (flag("--no-mic")) cli.set("audio.mic", "false");
    else if (flag("--no-vsync")) cli.set("video.vsync", "false");
    else if (flag("--interp")) cli.set("emu.jit", "false");
    else if (flag("--lockstep")) cli.set("emu.quantum", std::to_string(ds::LOCKSTEP_QUANTUM));
    else if (arg("--quantum")) cli.set("emu.quantum", argv[++i]);
    else if (flag("--timing-oc")) cli.set("emu.timing_oc", "true");
    else if (flag("--gx-worker")) cli.set("emu.gx_worker", "true");   // the geometry worker alone: FIFO, stall and swap timing kept, commands executed off-thread with the cull priced by the previous frame's ratio
    else if (flag("--cpu-oc")) cli.set("emu.cpu_tuning", "overclock");
    else if (flag("--cpu-uc")) cli.set("emu.cpu_tuning", "underclock");
    else if (flag("--fast-load")) cli.set("emu.fast_load", "true");
    else if (arg("--frameskip")) cli.set("emu.frameskip", argv[++i]);
    else if (arg("--frameskip-mode")) cli.set("emu.frameskip_mode", argv[++i]);
    else if (flag("--frameskip-capture")) cli.set("emu.frameskip_capture", "true");
    else if (flag("--no-frameskip-capture")) cli.set("emu.frameskip_capture", "false");
    else if (flag("--aa")) cli.set("video.aa", "true");
    else if (flag("--no-aa")) cli.set("video.aa", "false");
    // The two halves of Timing OC separately: they pull in opposite directions
    // on Golden Sun, so the bundled flag reads flat while neither half is.
    else if (flag("--help")) { std::fputs(kUsage, stderr); return 0; }
    else if (argv[i][0] == '-' && argv[i][1] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); std::fputs(kUsage, stderr); return 2; }
    else rom = argv[i];
  }
  // DSiWare named without --dsi-mode runs the DSi machine too: as a DS it
  // would not start at all.
  const bool dsi_mode_asked = dsi_mode;   // --dsi-mode itself, not a DSiWare title implying it
  // A NAND title shortcut runs the title from the NAND with the hand-off.
  const bool rom_shortcut = rom && ds::io::is_shortcut_name(rom);
  if (!dsi_mode && rom && !dsi_nand && (rom_shortcut || ds::io::file_is_dsiware(rom))) dsi_mode = true;
  ds::sdl::Config cfg;
  const std::string global_ini = config_arg ? std::string(config_arg) : ds::sdl::Config::global_path();
  if (!config_arg) ds::sdl::Config::write_default(global_ini);
  if (!cfg.load(global_ini) && config_arg) { std::fprintf(stderr, "cannot read %s\n", config_arg); return 2; }
  // paths.dsi_nand stands in for --dsi-nand, but only under --dsi-mode: DSiWare
  // named on its own or picked from the game list keeps the hand-off, which
  // needs no signed TMD and starts sooner. emu.dsi_hide_installed likewise
  // stands in for --dsi-hide-installed.
  const std::string dsi_nand_cfg = dsi_mode_asked && !dsi_nand ? cfg.str("paths.dsi_nand") : std::string();
  if (!dsi_nand_cfg.empty()) dsi_nand = dsi_nand_cfg.c_str();
  if (!dsi_hide_installed) dsi_hide_installed = cfg.flag("emu.dsi_hide_installed", false);
  // The NAND a shortcut's title comes from: --dsi-nand, else paths.dsi_nand
  // (whether or not --dsi-mode was given).
  const std::string shortcut_nand = dsi_nand ? std::string(dsi_nand) : cfg.str("paths.dsi_nand");
  if (dsi_mode) {
    // A NAND boot to the launcher. A game named with it is DSiWare, installed
    // into the session's NAND rather than put in the card slot (a cart during
    // a NAND boot has not been checked against melonDS), so the rest of the
    // startup runs as the cartless firmware boot it is.
    dsi_hle = !dsi_nand && rom;
    if (!dsi_nand && !rom) { std::fprintf(stderr, "--dsi-mode needs --dsi-nand (or paths.dsi_nand in %s) or a DSiWare title\n", global_ini.c_str()); return 2; }
    if (dsi_hle && dsi_menu) { std::fprintf(stderr, "--dsi-menu needs --dsi-nand: without one there is no DSi Menu to boot\n"); return 2; }
    // Without a NAND the title goes in the slot and is handed over from there
    // (NDS::prepare_dsi_hle), so the ROM path stays as for any game.
    if (!dsi_hle) { dsi_title = rom; rom = nullptr; }
    // The recompiler, the event-bound interleave and idle skip apply as for a
    // DS game: checked against the interpreter on every DSiWare title on hand,
    // the NAND boot and a launch from the DSi Menu (docs/dsiware-scoping.md,
    // section 5 item 8). --interp and --lockstep still pick the reference.
  }
  auto apply_cli = [&] { for (const char* k : {"paths.bios9", "paths.bios7", "paths.firmware", "video.scale", "video.dual_window", "video.layout", "video.screen", "video.pip_alpha", "video.dominant_ratio", "video.dominant_threshold", "video.integer_scale",
                                              "video.fullscreen", "video.linear", "video.lcd_grid", "video.chunky", "video.chunky_threshold", "video.chunky_cell", "video.seam", "video.disp", "video.fbdev", "video.vsync", "audio.enabled", "audio.volume",
                                              "audio.mic", "emu.jit", "emu.quantum", "emu.speed", "emu.limiter", "emu.pacing", "audio.buffer_size", "audio.latency_frames", "emu.timing_oc", "emu.gx_worker", "emu.cpu_tuning", "emu.fast_load", "emu.frameskip", "emu.frameskip_mode", "emu.frameskip_capture", "video.aa", "emu.autosave_png", "emu.autoload", "cheevos.enabled", "cheevos.token_file", "cheevos.username"}) if (cli.has(k)) cfg.set(k, cli.str(k)); };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7");
  // The DSi's own firmware, for a session that is a DSi from the start, unless
  // --firmware named one; a path that names no file counts as unset.
  const std::string dsi_fw = cfg.str("paths.dsi_firmware");
  const bool have_dsi_fw = !dsi_fw.empty() && std::filesystem::exists(dsi_fw);
  const std::string fw = dsi_mode && have_dsi_fw && !cli.has("paths.firmware") ? dsi_fw : cfg.str("paths.firmware");
  // The DSi BIOS pair: needed for DSi mode, and for DSiWare picked from the
  // game list. A path that names no file counts as unset.
  const std::string dsi_bios9i = bios9i ? std::string(bios9i) : cfg.str("paths.bios9i");
  const std::string dsi_bios7i = bios7i ? std::string(bios7i) : cfg.str("paths.bios7i");
  auto have_dsi_bios = [&] { return !dsi_bios9i.empty() && !dsi_bios7i.empty() && std::filesystem::exists(dsi_bios9i) && std::filesystem::exists(dsi_bios7i); };
  if (dsi_mode && !have_dsi_bios()) {
    std::fprintf(stderr, "%s needs the DSi BIOS pair: --bios9i/--bios7i, or paths.bios9i/paths.bios7i in %s\n",
                 dsi_hle ? (std::string(rom) + " is DSiWare and").c_str() : "--dsi-mode", global_ini.c_str());
    return 2;
  }

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

  // The per-game files go on top of the global one, the command line on top of
  // both: title ID first, then the file named after the ROM, so the filename
  // wins -- that is also where hotkey- and menu-picked settings are remembered.
  //
  // This has to happen here, not when the cart is loaded, because everything
  // below reads `cfg` first: the display is opened, audio started and input
  // configured long before the ROM is in the slot. Merged there, a per-game
  // [keys], [pad] or video.scale was silently ignored -- even though the layout
  // hotkey writes video.layout into that very file. The merge runs again after
  // the cart is loaded, which is what covers a zip (whose game code cannot be
  // read without unpacking it) and costs nothing when the values are the same.
  auto merge_game_config = [&](const char* code) {
    std::vector<std::string> paths;
    if (code) paths.push_back(ds::sdl::Config::game_path_code(code));
    paths.push_back(ds::sdl::Config::game_path_rom(rom_path));
    for (const std::string& p : paths)
      if (!p.empty() && cfg.load(p)) VLOG("config: %s\n", p.c_str());
    apply_cli();
  };
  if (!boot_firmware) {
    char code[4];
    merge_game_config(peek_game_code(rom_path, code) ? code : nullptr);
  }

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
  // Core knobs the core reads from the environment. These must be set before
  // the NDS is constructed: Scheduler's constructor reads DS_IDLE_SKIP once
  // (scheduler.cpp), so setting it afterwards left emu.idle_skip a no-op.
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP")) setenv("DS_IDLE_SKIP", cfg.str("emu.idle_skip").c_str(), 1);
  if (cfg.num("emu.host_cores", 0) > 0 && !std::getenv("DS_HOST_CORES")) setenv("DS_HOST_CORES", cfg.str("emu.host_cores").c_str(), 1);
  VLOG("host: %u cores\n", ds::host_cores());

  NDS nds;
  ds::bios::UserSettings user;
  user.nickname = cfg.str("user.nickname", user.nickname);
  user.message = cfg.str("user.message", user.message);
  user.birthday_month = static_cast<ds::u8>(cfg.num("user.birthday_month", user.birthday_month));
  user.birthday_day = static_cast<ds::u8>(cfg.num("user.birthday_day", user.birthday_day));
  user.favourite_colour = static_cast<ds::u8>(cfg.num("user.colour", user.favourite_colour));
  user.language = static_cast<ds::u8>(cfg.num("user.language", user.language));
  // [net] net.mode -- the menu's NETWORK FEATURES row, and the only way to
  // reach local wireless without the command line. A --netplay or --lan-*
  // flag has already said what to do, so the config only speaks when they are
  // all silent. The name on the session is the console's own nickname unless
  // --lan-name overrode it: the name a game shows for this console and the
  // name its peers see are the same thing to whoever is reading the screen.
  std::string lan_name_str = lan_name;
  if (!lan_name_set && !user.nickname.empty()) lan_name_str = user.nickname;
  lan_name = lan_name_str.c_str();
  if (!lan_host && !lan_join && !netplay && !internet) {
    const std::string mode = cfg.str("net.mode", "off");
    if (mode == "auto") netplay = true;
    else if (mode == "host") lan_host = lan_name;
    else if (mode == "guest") lan_guest = true;
    else if (mode == "internet") internet = true;
    else if (mode != "off")
      std::fprintf(stderr, "net.mode: \"%s\" is not off, auto, host, guest or internet\n", mode.c_str());
  }
  // One radio, one use of it. The menu cannot ask for both -- NETWORK FEATURES
  // is a single row -- but the flags can, and a session that tried to be a
  // local peer and an internet client at once would have the access point
  // beaconing into a channel the MP transport is also driving.
  if (internet && (lan_host || lan_join || netplay || lan_guest)) {
    std::fprintf(stderr, "net: --internet is not local wireless; pick one\n");
    return 1;
  }
  {
    std::string err;
    if (!nds.load_bios(bios9, bios7, fw, user, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
    if (!nds.firmware_synthetic) VLOG("firmware: %s\n", fw.c_str());
    // A MAC of this console's own, always -- not only when a session was asked
    // for on the command line. Every firmware dump carries the MAC of the
    // console it came off, and the generated one has a fixed 00:09:BF:11:22:33,
    // so two DSperate instances sharing either would share a MAC. That is the
    // fault that made PictoChat drop its own messages, and it is why this was
    // once done only for a session; doing it every run instead is what lets a
    // session start at any time, including from the menu mid-game, because the
    // game reads the MAC out of the firmware when it brings its radio up.
    //
    // Generated once and kept in the config, rather than rolled each boot: the
    // low three bytes are the only part a DS lets differ, and a DS presents its
    // MAC as identity in one place -- the `macadr` the NAS login sends beside
    // the user ID held in the game's save. Whether Wiimmfi minds that changing
    // is untested, and a stable value costs nothing.
    {
      u32 suffix = static_cast<u32>(cfg.num("net.mac_suffix", -1));
      if (suffix > 0xFFFFFF) {
        std::random_device rd;
        suffix = rd() & 0xFFFFFF;
        if (!ds::sdl::Config::store(global_ini, "net.mac_suffix", std::to_string(suffix)))
          std::fprintf(stderr, "net: could not remember this console's MAC in %s\n", global_ini.c_str());
        cfg.set("net.mac_suffix", std::to_string(suffix));
      }
      nds.set_wifi_mac_suffix(suffix);
    }
  }
  // A configured path that names no file falls back the same as none: the
  // stock ini on a handheld points at files the user may never add.
  if (!nds.bios_native) std::fprintf(stderr, "bios: %s, using the built-in FreeBIOS (direct boot only; timing is not Nintendo's)\n", bios9.empty() ? "no dumps given" : "dumps not found");
  if (nds.firmware_synthetic) std::fprintf(stderr, "firmware: %s, using a generated one ([user] in %s)\n", fw.empty() ? "no dump given" : "dump not found", global_ini.c_str());
  if (boot_firmware && !nds.can_boot_firmware()) {
    std::fprintf(stderr, "the DS menu needs real dumps: %s%s%s (--bios9/--bios7/--firmware or [paths] in %s)\n",
                 nds.bios_native ? "" : "bios9 and bios7", (!nds.bios_native && nds.firmware_synthetic) ? " and " : "",
                 nds.firmware_synthetic ? "firmware" : "", global_ini.c_str());
    return 2;
  }
  // A DSi session needs all four dumps, not just the DSi pair. FreeBIOS is a
  // direct-boot stand-in for the DS ARM9/ARM7 BIOS and has neither the
  // Blowfish key table the cart's secure area is decrypted with nor the SWI
  // behaviour the launcher hand-off runs through -- a DSiWare title started
  // on it printed "secure area decryption failed" and then wedged a few
  // frames in, which is a worse answer than refusing. The DSi pair is gated
  // separately and earlier, before anything is loaded.
  if (dsi_mode && !nds.bios_native) {
    std::fprintf(stderr, "a DSi session needs the DS BIOS dumps as well as the DSi pair: %s (--bios9/--bios7 or [paths] in %s).\n"
                 "The built-in FreeBIOS cannot boot one.\n",
                 bios9.empty() ? "no dumps given" : "dumps not found", global_ini.c_str());
    return 2;
  }
  // The DSi's SD card: a host folder, built into a card before the reset that
  // puts it in the slot (io/dsi_sd_card.h). Opened once; a later DSiWare pick
  // from the game list keeps it.
  const std::string dsi_sd_dir = dsi_sd ? std::string(dsi_sd) : cfg.str("paths.dsi_sd");
  auto open_dsi_sd = [&]() -> bool {
    if (dsi_sd_dir.empty() || nds.dsi_sd.valid()) return true;
    ds::io::SdCard::Report r;
    std::string err;
    if (!nds.dsi_sd.open(dsi_sd_dir, &r, &err)) { std::fprintf(stderr, "dsi sd: %s\n", err.c_str()); return false; }
    VLOG("dsi sd: %s, %d files and %d folders on a %llu MB FAT%d card\n", dsi_sd_dir.c_str(), r.files, r.dirs,
         static_cast<unsigned long long>(nds.dsi_sd.length() >> 20), nds.dsi_sd.fat_bits());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
    return true;
  };
  if (dsi_hle) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) { std::fprintf(stderr, "dsi bios: %s\n", err.c_str()); return 1; }
    if (!nds.bios_native_dsi) { std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str()); return 1; }
    if (!open_dsi_sd()) return 1;
    if (rom_shortcut) {
      if (shortcut_nand.empty()) { std::fprintf(stderr, "%s is a NAND title shortcut and needs the NAND: --dsi-nand or paths.dsi_nand in %s\n", rom, global_ini.c_str()); return 1; }
      if (!nds.load_dsi_nand(shortcut_nand, &err)) { std::fprintf(stderr, "dsi nand: %s\n", err.c_str()); return 1; }
    }
    nds.set_dsi(true);
    nds.dsi_hle_launch = true;   // the NAND and settings are made once the title is in the slot (below)
    std::fprintf(stderr, rom_shortcut ? "console: DSi, a title from its NAND without the DSi Menu (EXPERIMENTAL)\n" : "console: DSi without a NAND (EXPERIMENTAL)\n");
  } else if (dsi_mode) {
    std::string err;
    if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err)) { std::fprintf(stderr, "dsi bios: %s\n", err.c_str()); return 1; }
    if (!nds.bios_native_dsi) { std::fprintf(stderr, "dsi bios: %s or %s not found\n", dsi_bios9i.c_str(), dsi_bios7i.c_str()); return 1; }
    if (!open_dsi_sd()) return 1;
    // Opened read-only: the guest's writes (TWLCFG, title saves) are held in
    // memory (NandImage), so the dump is never written and nothing is kept
    // between runs until saves and settings are pulled out as files.
    if (!nds.load_dsi_nand(dsi_nand, &err)) { std::fprintf(stderr, "dsi nand: %s\n", err.c_str()); return 1; }
    VLOG("dsi nand: %s (read-only; writes held in memory)\n", dsi_nand);
    const ds::io::NandPersistPaths pp = ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves"));
    if (dsi_hide_installed) {
      const int n = ds::io::nand_hide_installed_dsiware(nds.dsi_nand, nds.bus.bios7i.get(), &err);
      if (n < 0) { std::fprintf(stderr, "dsi: hiding the installed titles: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "dsi: %d installed titles hidden for this session\n", n);
    }
    // The DSiWare title named on the command line, into the session: at most
    // one, and only when the NAND does not have it already. The launcher starts
    // it only with its Nintendo-signed TMD: the CIA's own, the one cached
    // beside the saves, a download from the update CDN, or <game>.tmd.
    if (dsi_title) {
      std::vector<ds::u8> srl, embedded;
      if (!ds::io::read_dsiware(dsi_title, srl, &err, &embedded)) { std::fprintf(stderr, "dsi: %s\n", err.c_str()); return 2; }
#if DSPERATE_CHEEVOS
      // Only for the autoload: with --dsi-menu the player picks what runs, and
      // this title may never start.
      if (cfg.flag("cheevos.enabled", false) && !dsi_menu) {
        const auto src = ds::cart::RomSource::from_memory(srl);
        std::string herr;
        if (!src || !ds::cheevos::rom_hash(*src, dsi_title, dsi_title_cheevos_hash, herr, true))
          std::fprintf(stderr, "cheevos: cannot identify %s: %s\n", dsi_title, herr.c_str());
      }
#endif
      const u32 lo = static_cast<u32>(srl[0x230] | (srl[0x231] << 8) | (srl[0x232] << 16) | (srl[0x233] << 24));
      if (ds::io::nand_has_title(nds.dsi_nand, nds.bus.bios7i.get(), lo)) {
        std::fprintf(stderr, "dsi: %.4s: already installed on the NAND\n", reinterpret_cast<const char*>(&srl[0x0C]));
        dsi_title_lo = lo;
      } else {
        std::string beside = dsi_tmd ? std::string(dsi_tmd) : rom_stem(std::string(dsi_title)) + ".tmd";
        const std::string cache = pp.saves_dir + "/" + std::string(reinterpret_cast<const char*>(&srl[0x0C]), 4) + ".tmd";
        ds::io::TmdFetch fetch;
#if DSPERATE_CHEEVOS
        std::string http_err;
        std::shared_ptr<ds::cheevos::Backend> http = dsi_offline ? nullptr : std::shared_ptr<ds::cheevos::Backend>(ds::cheevos::make_curl_backend(http_err));
        if (http) fetch = [http](const std::string& url, std::vector<ds::u8>& body) {
          const ds::cheevos::Response resp = http->perform({url, {}, {}}, "DSperate");
          if (resp.status != 200) return false;
          body.assign(resp.body.begin(), resp.body.end());
          return true;
        };
#endif
        std::vector<std::string> tmd_log;
        const std::vector<ds::u8> tmd = ds::io::find_signed_tmd(srl, embedded, cache, beside, fetch, tmd_log);
        for (const std::string& l : tmd_log) VLOG("dsi: %s\n", l.c_str());
        if (tmd.empty()) {
          for (const std::string& l : tmd_log) std::fprintf(stderr, "dsi: %s\n", l.c_str());
          std::fprintf(stderr, "dsi: no signed DSi TMD for %.4s; the launcher cannot start it without one (put it at %s%s)\n",
                       reinterpret_cast<const char*>(&srl[0x0C]), beside.c_str(), fetch ? "" : ", or allow the download");
          return 2;
        }
        const ds::io::TitleInstall r = ds::io::nand_install_title(nds.dsi_nand, nds.bus.bios7i.get(), srl, tmd);
        std::fprintf(stderr, "dsi: %.4s: %s\n", reinterpret_cast<const char*>(&srl[0x0C]), r.message.c_str());
        if (r.result == ds::io::TitleInstall::Result::Failed) return 1;
        dsi_title_lo = r.title_lo;
      }
    }
    // What earlier sessions carried out -- title saves, the system sidecar,
    // photos -- goes back in before the boot reads the NAND. Not under a
    // replay, which has to start from the same console every time. A save
    // state carries the NAND's sectors from the point before (NandImage::
    // mark_state_base), so it brings back the saves it was made with.
    nds.dsi_nand.mark_state_base();
    if (!replay) {
      const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), pp);
      if (r.saves || r.system_files || r.photos) std::fprintf(stderr, "dsi: restored %d title saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
      for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
    }
    nds.set_dsi(true);
    nds.dsi_nand_boot = true;   // reset() below builds the DSi machine; setup_direct_boot() then boots the NAND
    std::fprintf(stderr, "console: DSi (EXPERIMENTAL)\n");
  }
  nds.reset();
  // The firmware writes its settings pages to flash over SPI. Those go to a
  // sidecar beside the firmware rather than into the dump itself, so a rename
  // in the DS menu survives a restart without the emulator ever writing to a
  // file the user cannot regenerate. See NDS::load_firmware_override.
  // None in DSi mode: like its NAND copy, a DSi session keeps nothing yet, and
  // the DSi firmware's sidecar would otherwise land beside the user's dump.
  std::string fw_override = dsi_mode ? std::string() : cfg.str("paths.firmware_override", fw + ".ovr");
  if (!nds.firmware_synthetic && !fw_override.empty()) {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) {
      if (err != "cannot open") std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    } else if (err.empty()) {
      VLOG("firmware settings: %s\n", fw_override.c_str());   // loaded cleanly: chatter
    } else {
      std::fprintf(stderr, "firmware settings: %s -- warning: %s\n", fw_override.c_str(), err.c_str());
    }
  }
  // [video] in one place; see VideoSetup. The references below keep the rest of
  // this function reading as it did, and are what a settings change re-fills.
  VideoSetup vs;
  if (!parse_video(cfg, vs)) return 2;
  const u32& grid = vs.grid;
  const u8& seam_blend = vs.seam_blend;
  u8& chunky = vs.chunky;
  const u32& chunky_thresh = vs.chunky_thresh;
  bool audio_on = cfg.flag("audio.enabled", true), mic_on = cfg.flag("audio.mic", true);
  const bool jit = cfg.flag("emu.jit", true);
  const bool& dual_window = vs.dual_window;
  const long quantum = cfg.num("emu.quantum", 0);   // event-bound interleave (DraStic's rule): 5-10 % faster than lockstep
  using Disp = ds::sdl::Display;
  using Menu = ds::sdl::Menu;
  Disp::Layout& layout = vs.layout;
  std::vector<Disp::Mode>& layout_cycle = vs.layout_cycle;
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
  bool& use_disp = vs.use_disp;
  use_disp = disp_mode == "true" || disp_mode == "on";
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
  bool& use_fbdev = vs.use_fbdev;
  use_fbdev = fbdev_mode == "true" || fbdev_mode == "on";
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
  if (dual_window) {
    if (SDL_GetNumVideoDisplays() < 2) { std::fprintf(stderr, "--dual-window needs two video displays\n"); SDL_Quit(); return 1; }
    // Which display is the physical bottom panel depends on the driver, both
    // verified on the dual-panel board: under KMSDRM display 0 is DSI-1,
    // which is -- unintuitively -- the lower panel, while sway's canvas
    // arranges the outputs the other way around.
    const char* vd = SDL_GetCurrentVideoDriver();
    vs.bottom_display = vd && !std::strcmp(vd, "KMSDRM") ? 0 : 1;
  }
  if (!open_displays(vs, display, display2)) { SDL_Quit(); return 1; }
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
  // [audio] buffer_size is milliseconds. It replaced latency_frames, which
  // counted whole DS frames; that key is still read when buffer_size is unset,
  // so an ini or a per-game file written before the rename keeps its setting
  // rather than silently reverting to the default.
  // "auto" (the default) lets it move the target itself, within the one
  // regime where depth is the answer -- see Audio::set_buffer_auto.
  auto apply_audio_buffer = [&audio](const std::string& v) {
    if (v.empty() || v == "auto") { audio.set_buffer_auto(); return; }
    audio.set_buffer_ms(std::atof(v.c_str()));
  };
  if (const std::string v = cfg.str("audio.buffer_size"); !v.empty()) apply_audio_buffer(v);
  else if (const int frames = cfg.num("audio.latency_frames", 0); frames > 0)
    audio.set_buffer_ms(frames * ds::sdl::Audio::FRAME_MS);
  else audio.set_buffer_auto();
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
      ds::gpu::Gpu::ScaleTarget st = plain
          ? ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun_plain}
          : ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun, at_source || !target[i].grid ? 256u : grid, display.chunky_on(i) ? chunky : static_cast<u8>(0), chunky_thresh,
                                      at_source ? static_cast<u8>(0) : seam_blend, target[i].seam_w,
                                      static_cast<const ds::gpu::Gpu::CellMap*>((dual_window && i == vs.bottom_display ? display2 : display).cell_map(i)),
                                      vs.linear && !at_source, target[i].lin_sx, target[i].lin_wx};
      st.y_lo = target[i].y_lo; st.y_hi = target[i].y_hi;   // the crop window (integer overscale)
      nds.gpu.set_scale_target(i, st);
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
      const char* const line3 = "FIRST LAUNCH ONLY    B CANCELS";
      // The notice goes where the pause menu goes: on the panel's own pixels
      // where they can be written, so it is laid out for the screen it is
      // actually on and comes out at panel resolution rather than being
      // upscaled with the held frame. The display-engine and SDL_Renderer
      // tiers keep the 256x192 scratch, which goes through the scaler.
      const bool on_canvas = display.canvas_capable();
      if (!on_canvas) ds::sdl::draw_notice(ds_canvas(menu_fb[menu_screen].data()), title.c_str(), line2, line3);
      ds::sdl::Display::Target target[2] = {};
      bool scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      if (scaled) {
        set_scale_targets(target, true, true);
        for (int i = 0; i < 2; ++i) nds.gpu.scale_image(i, menu_fb[i].data());
        // Before the notice, so a PiP inset cannot land on top of it.
        display.finish_views();
        if (dual_window) display2.finish_views();
        ds::sdl::Display::CanvasView cv;
        if (on_canvas && display.canvas(cv)) {
          ds::sdl::draw_notice(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h}, title.c_str(), line2, line3);
          // The whole canvas: the dots repaint the panel every step anyway,
          // and the letterbox around it is not redrawn by anything else.
          display.note_canvas_draw_all();
        }
        display.present();
        if (dual_window) display2.present();
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
  const bool rom_is_cia = [&] {
    if (rom_path.size() < 4) return false;
    std::string ext = rom_path.substr(rom_path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".cia";
  }();
  // A shortcut's title comes out of the NAND loaded above.
  auto load_shortcut_title = [&](const std::string& path) -> bool {
    ds::io::NandShortcut sc;
    std::string err;
    if (!ds::io::read_shortcut(path, sc)) { std::fprintf(stderr, "dsi: %s is not a NAND title shortcut\n", path.c_str()); return false; }
    if (!sc.from(nds.dsi_nand)) { std::fprintf(stderr, "dsi: %s was made from another NAND than %s\n", path.c_str(), shortcut_nand.c_str()); return false; }
    if (!nds.load_dsi_nand_title(sc.title_lo, &err)) { std::fprintf(stderr, "dsi: %s: %s\n", path.c_str(), err.c_str()); return false; }
    return true;
  };
  if (dsi_hle && rom_shortcut) {
    if (!load_shortcut_title(rom_path)) { SDL_Quit(); return 1; }
  } else if (dsi_hle && rom_is_cia) {
    // A CIA carries the SRL inside; the slot takes the SRL.
    std::vector<ds::u8> srl;
    std::string err;
    if (!ds::io::read_dsiware(rom_path, srl, &err) || !nds.load_rom_image(std::move(srl))) { std::fprintf(stderr, "dsi: %s: %s\n", rom_path.c_str(), err.c_str()); SDL_Quit(); return 1; }
  } else if (!boot_firmware && !load_rom_notice(rom_path)) { std::fprintf(stderr, "could not read %s\n", rom_path.c_str()); SDL_Quit(); return 1; }
  // The launcher hand-off's made-up console for the DSiWare in the slot:
  // settings from [user], the font, the NAND (NDS::prepare_dsi_hle).
  auto prepare_dsiware = [&]() -> bool {
    std::string err;
    std::vector<std::string> made;
    nds.dsi_font_path = dsi_font ? std::string(dsi_font) : cfg.str("paths.dsi_font");
    if (!nds.dsi_font_path.empty() && !std::filesystem::exists(nds.dsi_font_path)) {
      std::fprintf(stderr, "dsi: font %s not found; using DSperate's own\n", nds.dsi_font_path.c_str());
      nds.dsi_font_path.clear();
    }
    if (!nds.prepare_dsi_hle(user, &err, &made)) { std::fprintf(stderr, "dsi: %s\n", err.c_str()); return false; }
    for (const std::string& m : made) std::fprintf(stderr, "dsi: %s\n", m.c_str());
    return true;
  };
  if (dsi_hle && !prepare_dsiware()) { SDL_Quit(); return 1; }
  // On a firmware boot the loader cart goes in the slot. A BootMenu.nds beside
  // the config wins if there is one -- that is how a hand-made card from
  // tools/mkcart.py is used -- and otherwise the built-in one is assembled in
  // memory, so the emulator needs no file shipped alongside it. Its two banner
  // lines are the only part worth configuring; a different icon means building
  // a card with the script.
  // The DSi launcher takes the same card (its whitelist refuses it, but only
  // after the launch fade the picker stops on; see NDS::dsi_loader_watch).
  // Not while a DSiWare title is being autoloaded: a card in the slot of that
  // boot has not been tried.
  if (boot_firmware && (!dsi_mode || (!dsi_hle && (!dsi_title || dsi_menu)))) {
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
  // The same merge again, now that the cart's real game code is known. For a
  // plain .nds this repeats what the early merge above already did; for a zip,
  // whose code could not be read without unpacking it, this is the first time
  // the title-ID file is seen. Anything read before this point (display, audio,
  // input) therefore takes a zip's per-game settings only from the filename file.
  if (nds.cart) {
    merge_game_config(nds.cart->header().game_code);
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
  // Geometry worker + per-frame shape controller, with either inexact tier
  // (no-FIFO, or the FIFO kept with the cull priced by ratio under --cpu-oc).
  nds.gpu3d.set_geometry_worker(cfg.flag("emu.timing_oc", false) || cfg.flag("emu.gx_worker", false) || cpu_oc_mode(cfg.str("emu.cpu_tuning")) != 0);   // DS_GX_THREAD: 0 never, 1 per-frame shape controller, 2 always
  nds.io.set_cart_bulk(cfg.flag("emu.fast_load", false));   // may introduce accuracy issues, see config.cpp
  nds.gpu3d.renderer().set_aa(cfg.flag("video.aa", false));   // opt-in: see config.cpp
  if (!boot_firmware || nds.dsi) nds.setup_direct_boot();   // on a DSi this is the NAND boot (NDS::boot_dsi_nand)
  // A DSiWare title given with --dsi-mode starts straight away (the TLNC
  // autoload the launcher reads), unless --dsi-menu asks for the menu with it.
  if (nds.dsi && dsi_title_lo && !dsi_menu) nds.dsi_autoload(dsi_title_lo);
  // A real console's clock, seeded from this machine. Off in the core by
  // default so the verification harness stays reproducible; a frontend
  // showing someone their own DS menu wants the real date on it. Not under
  // --replay: a recorded scene has to reproduce frame for frame, and a game
  // that reads the date (Animal Crossing, the Pokemon day/night cycle) would
  // otherwise play differently every time it was replayed.
  if (!replay || rtc_host) nds.io.start_rtc_clock();
  else VLOG("rtc: frozen for the replay\n");
  if (replay && rtc_host) std::fprintf(stderr, "rtc: --rtc-host over a replay; this run is not reproducible\n");
  // emu.cpu_tuning as set, and as the recompiler runs it (cpu_tuning_for).
  // Checked once a frame below, so it follows the DSi Menu starting a title
  // and the title returning to the Menu.
  int cpu_oc_cfg = cpu_oc_mode(cfg.str("emu.cpu_tuning"));
  int cpu_oc_applied = 0;
  auto cpu_oc_wanted = [&](int mode) { return cpu_tuning_for(nds, mode); };
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
  if (jit) ds::jit::set_cpu_oc(static_cast<ds::jit::CpuOc>(cpu_oc_applied = cpu_oc_wanted(cpu_oc_cfg)));   // see config.cpp; translate-time pricing, so before the first block
  // The firmware boots under per-instruction budget checks. Block-granularity
  // overshoot has been seen to stop it booting at all on the RG DS -- not
  // every time, which is what a timing race looks like -- and the console is
  // idle enough there that the cost of checking does not show. It is dropped
  // again the moment a game is launched, where it very much would.
  // The DS menu needs the recompiler's strict timing until a game launches.
  // Not the DSi Menu: it runs, and launches its titles, without it (checked
  // against the interpreter), and a title it starts would otherwise keep it
  // for the whole session -- nothing turns it off there.
  if (jit && boot_firmware && !nds.dsi) { ds::jit::set_strict(true); VLOG("jit: strict timing for the firmware\n"); }
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
  // An explicit --load-state wins; the auto slot is only what is fallen back
  // to. Skipped under a replay (its inputs run from boot, so a state would
  // desynchronise it) and a recording (the same reason autosave_now() skips
  // them: the log is the inputs from boot).
  const std::string autoload = load_state || log.reading() || log.writing()
                                   ? std::string()
                                   : autoload_path(nds, session.states_dir, cfg.flag("emu.autoload", false));
  if (load_state || !autoload.empty()) {
    const char* start_state = load_state ? load_state : autoload.c_str();
    bool got_layout = false;
    // A --load-state that cannot be read is the player's explicit request and
    // fails the run; an unreadable auto state (truncated by a SIGKILL
    // mid-write, or written by an older build) only means there is nothing to
    // resume, so the game boots as usual. load_state_file() has said why.
    if (!load_state_file(nds, start_state, layout, got_layout)) {
      if (load_state) return 1;
    } else {
      if (!load_state) std::fprintf(stderr, "state: autoloaded %s\n", start_state);
      // The state's view replaces the config's, dual-window aside (two panels
      // show both screens, whatever the state says).
      if (got_layout && !dual_window) { display.set_layout(layout); apply_visibility(); }
      // A replay continues from the state's frame, not from the log's start.
      if (log.reading()) { ds::input::Frame f; for (u64 k = 0; k < nds.frame_count && log.read(f); ++k) {} }
    }
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
  ds::sdl::Pacer pacer(frame_ns);
  // emu.pacing -- how the wait to the next deadline is spent.
  //
  // "sleep" is the obvious answer and the right one on any machine whose
  // clock does not depend on what the last few milliseconds looked like. On
  // one whose does, sleeping is what makes the emulator slow: a polling
  // governor reads the sleep as idle and clocks down under us (cpu_gov.h has
  // the measurements). "busy" holds the core to the deadline instead, which
  // costs a core's idle power and buys back the dives without touching a
  // single system-wide setting -- nothing here writes to sysfs, it only
  // reads which governor is in charge.
  //
  // "auto", the default, is "busy" exactly when a polling governor is in
  // charge, and a plain sleep otherwise.
  {
    const std::string pacing = cfg.str("emu.pacing", "auto");
    std::string gov;
    const bool polls = ds::sdl::host_governor_polls(&gov);
    const bool busy = pacing == "busy" || (pacing != "sleep" && pacing != "off" && polls);
    pacer.set_busy_wait(busy);
    if (busy) VLOG("pacing: busy-wait to the deadline (governor %s%s)\n",
                   polls ? gov.c_str() : ds::sdl::cpu_governor().c_str(),
                   pacing == "busy" ? ", asked for" : ": it decides by polling load");
    else VLOG("pacing: sleep to the deadline (governor %s)\n", ds::sdl::cpu_governor().c_str());
  }
  // emu.rt_relief -- stay under the kernel's real-time bandwidth cap.
  //
  // Only worth anything to a thread that actually runs real-time (emu.realtime)
  // and only where the cap is on, which is every stock distro:
  // sched_rt_runtime_us/sched_rt_period_us, 950000 of 1000000 us on ROCKNIX.
  // The pacer's comment has the reasoning and the numbers; the short version is
  // that the kernel takes its 5 % in one ~50 ms lump, and a scene with no slack
  // wears it as a hitch every second or so. Default on, off for an A/B.
  if (cfg.flag("emu.rt_relief", true)) {
    const auto sysctl_num = [](const char* path) -> double {
      std::FILE* f = std::fopen(path, "r");
      if (!f) return 0.0;
      double v = 0.0;
      if (std::fscanf(f, "%lf", &v) != 1) v = 0.0;
      std::fclose(f);
      return v;
    };
    const double runtime_us = sysctl_num("/proc/sys/kernel/sched_rt_runtime_us");
    const double period_us = sysctl_num("/proc/sys/kernel/sched_rt_period_us");
    int policy = 0; sched_param sp{};
    const bool rt = pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && (policy == SCHED_RR || policy == SCHED_FIFO);
    if (!rt) VLOG("rt relief: not needed, this thread is not real-time\n");
    else if (runtime_us < 0 || period_us <= 0) VLOG("rt relief: not needed, the real-time class is uncapped\n");
    else if (runtime_us >= period_us) VLOG("rt relief: not needed, the cap is not a cap (%.0f of %.0f us)\n", runtime_us, period_us);
    else {
      pacer.set_rt_relief(runtime_us / period_us, period_us * 1000.0);
      VLOG("rt relief: the real-time class is capped at %.1f %% of every %.0f ms; giving the rest back a frame at a time\n",
           100.0 * runtime_us / period_us, period_us / 1000.0);
    }
  }
  // The limiter: what rate the emulator is held to, and how fast the game
  // runs against it.
  //
  // "auto" is the console's own 59.8261 Hz and is the default: it is the only
  // value that plays a game at the speed it was written for, and the speed
  // everything else here is built around -- the SPU's own rate, the audio
  // rate control's trim, the length of a replay. The rest are the rates a
  // panel comes in. 60 is what a player means by "60 fps" and costs 0.29 %
  // fast -- a third of a second an hour, and a pitch shift of five cents
  // that the rate control absorbs without complaint; what it buys on a 60 Hz
  // panel is lining up with the refresh, where the console's own rate slips
  // a frame against it about every six seconds. 30 halves the speed; 120 and
  // up run the game fast, the same axis fast forward uses.
  // "off" does not pace at all: unlike fast forward it still presents every
  // frame and still plays sound, so it is a limiter switched off rather than
  // a mode.
  //
  // emu.speed multiplies whatever that comes to, so 60 at 50 % and 30 at
  // 100 % are the same clock reached two ways.
  int speed_pct = cfg.num("emu.speed", 100);
  std::string limiter_mode = cfg.str("emu.limiter", "auto");
  bool limiter_off = false;
  // Everything that sets the machine's pace ends up here, as one number: how
  // many times the console's own rate it is running at. The pacer's period
  // is always one console frame and this scales it, so the limiter, the
  // speed percent and fast forward are three ways of writing the same thing
  // rather than three mechanisms.
  double base_scale = 1.0;
  double frame_budget_ms = frame_ns / 1e6;   // one frame at the rate now in force; adaptive frameskip's budget
  auto apply_limiter = [&] {
    // Anything unrecognised is the console's own rate: an ini that says
    // "59.8261" or "ds" gets what it plainly meant, and a typo gets the
    // faithful default rather than a wrong number.
    double hz = 0.0;
    if (limiter_mode == "off" || limiter_mode == "unlimited") { limiter_off = true; }
    else {
      limiter_off = false;
      const double parsed = std::atof(limiter_mode.c_str());
      hz = parsed >= 1.0 && parsed <= 1000.0 ? parsed : 0.0;
    }
    const double period_ns = hz > 0.0 ? 1e9 / hz : frame_ns;
    // Held to the documented 25..400. The menu once wrote 1 for what it showed
    // as 100%, and one percent looks like a machine that does not boot.
    speed_pct = speed_pct > 0 ? std::clamp(speed_pct, 25, 400) : 100;
    const double scale = speed_pct / 100.0;
    base_scale = frame_ns / period_ns * scale;
    frame_budget_ms = period_ns / 1e6 / scale;
    // The audio has to be stretched or squeezed by however far this is from
    // the console's own rate, or the rate control spends the session pinned
    // to its clamp trying to make up a factor it cannot reach. An unlimited
    // limiter is the exception: it runs as fast as the machine goes, which
    // is not a number, so the audio keeps the console's rate and the drop
    // path deals with the surplus, exactly as it does for fast forward.
    audio.set_speed(limiter_off ? 1.0 : base_scale);
    pacer.reset();
  };
  apply_limiter();
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
  Uint64 emu_ticks = 0, draw_ticks = 0, wait_ticks = 0, fps_pace_ticks = 0;   // wait: blocked in begin_frame for a free scanout buffer
  u64 frames = 0;
  // Per-frame emulation time, for the same report the headless frontend prints. Only the
  // run_frame() slice goes in: the present blocks on vsync and the limiter
  // sleeps, and either one would peg every frame at the refresh interval and
  // hide exactly the clusters this is here to find.
  const double ticks_to_ms = 1e3 / static_cast<double>(SDL_GetPerformanceFrequency());
  std::vector<double> frame_ms, work_ms;   // emu slice; emu + present slice
  if (frame_limit > 0) { frame_ms.reserve(static_cast<size_t>(frame_limit)); work_ms.reserve(static_cast<size_t>(frame_limit)); }
  Uint64 pace_ticks = 0, draw_ticks_total = 0;
  bool paused = false;
  // A network session is actually up -- local wireless with a peer, or the
  // internet through the access point. Set once the transport has started,
  // not from the flags: a session that failed to come up leaves the machine
  // ordinary, and so should its restrictions.
  //
  // The emulator cannot stop, rewind, run fast or cut corners on timing while
  // something outside it is keeping time. That rules out save states, fast
  // forward, frameskip and the three inexact speed knobs for the session, and
  // it is why the pause menu stops pausing. Everything hangs off this one
  // fact, and the rows off Dep::NetSession, which reads it through the host.
  bool net_live = false;
  int state_slot = 0;
  // Fast forward: the `fast_forward` hotkey while held, or the toggle (also
  // [emu] fast_forward = true to start that way). It sets no pace of its own
  // any more -- it asks the limiter for a floor of ff_speed times real time
  // (0 = as fast as the machine goes, whatever the limiter says); ff_skip presents
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
  int fs_limit = cfg.num("emu.frameskip", 0);            // the menu can change these
  bool fs_adaptive = cfg.str("emu.frameskip_mode", "adaptive") != "fixed";
  const bool fs_capture = cfg.flag("emu.frameskip_capture", true);
  nds.gpu.set_frameskip_capture(fs_capture);
  u64 fs_refused = 0;         // skips the core would not take (capture / display FIFO)
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
  bool autosave = cfg.flag("emu.autosave", false);
  // Its thumbnail: "true" puts <GAMECODE>.auto.png beside the state, any
  // other value is the file to write (a launcher names the picture its game
  // switcher looks for). Only taken when the state is written.
  const std::string autosave_png_cfg = cfg.str("emu.autosave_png", "false");
  const bool autosave_png = autosave_png_cfg != "false" && autosave_png_cfg != "0" && !autosave_png_cfg.empty();
  bool ff_toggle = cfg.flag("emu.fast_forward", false);
  int ff_speed = cfg.num("emu.ff_speed", 0), ff_skip = cfg.num("emu.ff_skip", 3);
  bool was_fast = false;
  std::vector<u32> cursor_fb(ds::SCREEN_W * ds::SCREEN_H);   // bottom screen with the pen crosshair
  std::vector<u32> osd_fb(ds::SCREEN_W * ds::SCREEN_H);      // the primary screen with the slot digit / FPS counter
  std::vector<u32> flash_fb[2] = {std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H), std::vector<u32>(ds::SCREEN_W * ds::SCREEN_H)};   // both screens under the screenshot flash
  int flash_left = 0;                                        // frames of screenshot flash still to show
  int slot_shown = 0;                                        // frames left to show the slot field
  std::string slot_text;                                     // ... and what it says
  auto show_slot = [&](const std::string& text) { slot_text = text; slot_shown = SLOT_OSD_FRAMES; };
  // PiP inset opacity: where it is now (0..255), frames of opacity left
  // after the last touch, and the hold length from the config.
  int pip_touch_hold = std::max(0, cfg.num("video.pip_touch_hold", 60));
  constexpr int PIP_FADE_STEP = 24;                          // ~10 frames rest to opaque
  int pip_alpha = static_cast<int>(layout.pip_alpha * 255.0 + 0.5), pip_hold = 0;
  // The pause menu (menu.h) and the two screen copies it is composited into.
  ds::sdl::Menu menu;
  menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
  session.load_enabled(nds);
  // The library the loader cart's picker offers. Only read when there is a
  // loader cart to raise it: a normal session never shows the list.
  // NAND DSIWARE SHORTCUTS (emu.dsi_nand_shortcuts): a .dspr.nds file in the
  // shortcut folder (shortcuts_dir) for every DSiWare title on paths.dsi_nand while it is on,
  // none while it is off (io/dsi_nand_launch.h). Brought up to date at every
  // start, so a title installed or removed since is picked up, and when the
  // row is changed. Reads its own copy of the NAND: the session's is not
  // touched. Returns a one-line summary for the toast.
  auto sync_nand_shortcuts = [&](bool on) -> std::string {
    // A folder that paths.dsi_shortcuts has since moved away from keeps its
    // shortcuts: only the folder named now is brought up to date or cleared.
    const std::string dir = shortcuts_dir(cfg);
    if (dir.empty()) return "NO SHORTCUT FOLDER";
    ds::io::NandImage nand;
    std::vector<ds::u8> b7;
    if (on) {
      if (shortcut_nand.empty() || !std::filesystem::exists(shortcut_nand)) return "NO DSI NAND (PATHS.DSI_NAND)";
      if (!have_dsi_bios()) return "NO DSI BIOS";
      if (!nand.open(shortcut_nand)) return "THE DSI NAND CANNOT BE READ";
      std::ifstream f(dsi_bios7i, std::ios::binary);
      b7.assign(std::istreambuf_iterator<char>(f), {});
      if (b7.size() < 0x10000) return "THE DSI BIOS CANNOT BE READ";
    }
    const ds::io::ShortcutSync r = ds::io::sync_shortcuts(dir, on ? &nand : nullptr, on ? b7.data() : nullptr, on);
    if (r.written || r.removed) std::fprintf(stderr, "dsi shortcuts: %d written, %d removed in %s\n", r.written, r.removed, dir.c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi shortcuts: %s\n", n.c_str());
    return std::to_string(r.written) + " ADDED, " + std::to_string(r.removed) + " REMOVED";
  };
  if (cfg.flag("emu.dsi_nand_shortcuts", false)) sync_nand_shortcuts(true);
  std::string shortcuts_note;   // what the last change of the row did, for a toast
  std::vector<ds::sdl::Menu::GameEntry> games =
      boot_firmware && nds.cart ? enumerate_library(cfg) : std::vector<ds::sdl::Menu::GameEntry>{};
  menu.set_games(&games);
  // Armed until a game is launched: after that the cart in the slot is a real
  // one, and its own reads at its own arm9_rom_offset mean nothing.
  bool launcher = boot_firmware && nds.cart != nullptr;
  nds.dsi_loader_watch = launcher && nds.dsi;
  if (launcher) VLOG("launcher: %zu games in %s\n", games.size(), cfg.str("paths.games").c_str());
  bool launching = false;       // the card's launch fade is on screen; the list is going up
  bool launch_latched = false;  // ... and it has already been raised once for this fade
  // DS Download Play boots its downloaded program through the same fade to
  // white the card launch uses, so the picker went up over it -- and the pause
  // that came with it cut the session's timing dead. The Download Play guest
  // is a firmware boot with the loader cart in the slot, which is precisely
  // when `launcher` is armed; every Download Play run that proved the
  // transport was headless, where there is no picker, so nothing caught it.
  //
  // So: has this console been in a local-wireless exchange at all since it
  // booted. It cannot be tested at the white frame itself -- the host's Cut
  // Off arrives as a deauth, which clears the MP flags (Wifi's 0x00C0 path)
  // before the child is verified and booted, so by the time the fade lands
  // there is no live session left to see -- and it does not need to be, since
  // there is no way back from PictoChat or Download Play to the DS menu on
  // hardware: the player powers the console down. Once this console has
  // associated, the next fade to white is Download Play's child program and
  // not a card launch, for as long as this firmware keeps running.
  //
  // Cleared when the firmware reboots (the power-off path below), which is the
  // one way the DS menu comes back with the picker still armed.
  bool mp_ever = false;
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
  }
  // Carried onto the slot row: a state refused at boot (the auto slot loads
  // before there is a menu to tell) would otherwise show only as the game
  // starting from the beginning.
  menu.set_slot_notice(g_state_refused.empty() ? nullptr : g_state_refused.c_str()); };
  // Battery save flush: once the chip has been quiet for a second, and at
  // every point a session could end (pause, lid, quit).
  u32 sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
  u64 sram_quiet_since = 0;
  // DSi mode's equivalent: what the session wrote to the NAND goes out as
  // title saves, the system sidecar and photos (io/dsi_nand_persist), on the
  // same occasions and once NAND writes have been quiet for two seconds.
  // Without a NAND there is no dump to sit beside, and no system sidecar: the
  // DSi settings are [user], made again at every start.
  auto hle_paths = [&](const std::string& game) {
    ds::io::NandPersistPaths p;
    p.saves_dir = cfg.str("paths.saves");
    if (p.saves_dir.empty()) { const std::filesystem::path r(game); p.saves_dir = r.has_parent_path() ? r.parent_path().string() : "."; }
    p.photos_dir = p.saves_dir + "/dsi-photos";
    return p;
  };
  // A shortcut's title saves where a session on that NAND's DSi Menu does.
  ds::io::NandPersistPaths dsi_paths = rom_shortcut ? ds::io::NandPersistPaths::beside(shortcut_nand, cfg.str("paths.saves"))
                                     : dsi_hle ? hle_paths(rom_path)
                                     : dsi_mode ? ds::io::NandPersistPaths::beside(dsi_nand, cfg.str("paths.saves")) : ds::io::NandPersistPaths{};
  // The saves an earlier session carried out go back into a made-up NAND
  // before the title reads it; then that is the baseline the export compares.
  auto import_dsiware_saves = [&] {
    if (replay) return;
    const ds::io::NandPersistReport r = ds::io::nand_import(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    nds.dsi_nand.mark_baseline();
    if (r.saves || r.photos) std::fprintf(stderr, "dsi: restored %d title saves, %d photos\n", r.saves, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  if (dsi_hle) import_dsiware_saves();
  u64 nand_writes_seen = nds.dsi_nand.writes, nand_quiet_since = 0, nand_exported = nds.dsi_nand.writes;
  auto flush_dsi = [&] {
    if (!dsi_mode || save_readonly || nds.dsi_nand.writes == nand_exported) return;
    nand_exported = nds.dsi_nand.writes;
    const ds::io::NandPersistReport r = ds::io::nand_export(nds.dsi_nand, nds.bus.bios7i.get(), dsi_paths);
    if (r.saves || r.system_files || r.photos) VLOG("dsi: saved %d title saves, %d system files, %d photos\n", r.saves, r.system_files, r.photos);
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi: %s\n", n.c_str());
  };
  // The SD card's changes go back to its folder on the same occasions, and
  // once its writes have been quiet for two seconds.
  u64 sd_writes_seen = nds.dsi_sd.writes, sd_quiet_since = 0, sd_synced = nds.dsi_sd.writes;
  auto flush_sd = [&] {
    if (save_readonly || !nds.dsi_sd.valid() || nds.dsi_sd.writes == sd_synced) return;
    sd_synced = nds.dsi_sd.writes;
    const ds::io::SdCard::Report r = nds.dsi_sd.sync();
    if (r.files || r.dirs || r.removed) VLOG("dsi sd: %d files written, %d folders made, %d removed in %s\n", r.files, r.dirs, r.removed, nds.dsi_sd.folder().c_str());
    for (const std::string& n : r.notes) std::fprintf(stderr, "dsi sd: %s\n", n.c_str());
  };
  auto flush_save = [&] { if (!save_readonly && nds.cart && nds.cart->sram_dirty()) write_save(nds, session.sav); flush_dsi(); flush_sd(); };
  // The session is ending: leave a state behind. The same two guards the
  // save-state hotkey carries -- a replay must not write, and a recording is
  // the inputs from boot, so a state alongside it would only mislead.
  // Whether fb_ holds the picture of the last frame run: it does after an
  // unscaled frame (the pause stop, a screenshot frame, the renderer tier)
  // and not after a scanline-tier frame, whose lines went to the panel.
  bool fb_current = false;
  auto autosave_now = [&] {
    // net_live for the same reason the hotkey is refused: quitting out of a
    // session would otherwise leave a state that resumes into a conversation
    // whose other end is long gone. Read at call time, so a session that
    // never came up autosaves as usual.
    if (!autosave || save_readonly || log.writing() || net_live) return;
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
  // Reopen the display with whatever [video] now says. The tier and the
  // dual-window panel order are boot decisions and are carried over: choosing
  // them forces SDL's video driver, which is settled before SDL_Init. The
  // layout is carried over too, because the hotkeys may have moved it since
  // the file was read and the player would not expect a Visual FX change to
  // put the screens back.
  //
  // If the new settings will not open, the old ones are put back; if those
  // will not either there is no window to play in, so the session ends.
  auto reopen_display = [&]() -> bool {
    VideoSetup want;
    if (!parse_video(cfg, want)) return false;
    want.use_disp = vs.use_disp;
    want.use_fbdev = vs.use_fbdev;
    want.bottom_display = vs.bottom_display;
    want.layout = display.current_layout();
    const VideoSetup before = vs;
    display.close();
    if (dual_window) display2.close();
    vs = want;
    if (!open_displays(vs, display, display2)) {
      std::fprintf(stderr, "video: those settings would not open; keeping the old ones\n");
      display.close();
      if (dual_window) display2.close();
      vs = before;
      if (!open_displays(vs, display, display2)) { std::fprintf(stderr, "video: and neither will the old ones\n"); input.request_quit(); }
      return false;
    }
    apply_visibility();
    // A new window has forgotten that a text page is on it: the display-engine
    // tier suspends its chunky divisor while the menu is up, and without this
    // the menu would come back with its glyphs merged.
    display.set_page(paused);
    menu_dirty = true;
    return true;
  };

  // The pause menu's window onto the settings. Everything that knows what a
  // key means lives here; menu.cpp only knows how to draw a row and step a
  // value, the way the cheats page knows nothing about the cheat engine.
  struct Host final : ds::sdl::SettingsHost {
    ds::sdl::Config& cfg;
    NDS& nds;
    Disp& display;
    VideoSetup& vs;
    const std::string& global_ini;
    const std::string& game_ini;
    std::function<void(const char*, const std::string&)> apply;
    std::function<void()> reopen;
    bool per_game = false;
    // A session is up (Dep::NetSession). A pointer to the frontend's own flag
    // rather than a copy: the transports come up after this host is built,
    // and the menu must see the answer, not the guess.
    const bool* net_live = nullptr;

    Host(ds::sdl::Config& c, NDS& n, Disp& d, VideoSetup& v, const std::string& gi, const std::string& pi, ds::sdl::Input& in)
        : cfg(c), nds(n), display(d), vs(v), global_ini(gi), game_ini(pi), input(in) {}

    // The LAYOUT row reads the live layout, not the file: the hotkeys and a
    // loaded state move it without writing video.layout. Until commit() a
    // choice made on the row is held here, so the row shows what was picked
    // and the pip/dominant rows switch on for it straight away.
    std::string pending_mode;
    Disp::Mode effective_mode() const {
      Disp::Mode m;
      if (!pending_mode.empty() && Disp::parse_mode(pending_mode, m)) return m;
      return display.current_layout().mode;
    }
    // The cycle checkboxes: which mode a row names, or false for any other key.
    static bool cycle_key(const char* key, Disp::Mode& m) {
      const size_t n = std::strlen(ds::sdl::kLayoutCyclePrefix);
      return std::strncmp(key, ds::sdl::kLayoutCyclePrefix, n) == 0 && Disp::parse_mode(key + n, m);
    }
    bool in_cycle(Disp::Mode m) const {
      return std::find(vs.layout_cycle.begin(), vs.layout_cycle.end(), m) != vs.layout_cycle.end();
    }

    std::string get(const char* key) const override {
      if (is_user_key(key) && user_in_firmware()) return user_get(key);
      if (std::strcmp(key, "video.layout") == 0) return Disp::mode_name(effective_mode());
      Disp::Mode m;
      if (cycle_key(key, m)) return in_cycle(m) ? "true" : "false";
      return cfg.str(key, "");
    }

    void set(const char* key, const std::string& value) override {
      // The console's own settings live in the firmware, not the config, when
      // there is a dump to hold them: writing [user] there would change
      // nothing, since the dump's pages are what the console reads.
      if (is_user_key(key) && user_in_firmware()) { user_set(key, value); return; }
      // A checkbox edits the ring: the ring is what the file keeps, so that is
      // what gets applied and stored. Ticking a mode that is out of the ring
      // puts it back at its place in Display::Mode order among the ones that
      // are in, so the ring reads the same way whatever order it was built.
      Disp::Mode m;
      if (cycle_key(key, m)) {
        std::vector<Disp::Mode> ring = vs.layout_cycle;
        const bool on = value == "true";
        if (on && !in_cycle(m)) {
          auto at = ring.begin();
          while (at != ring.end() && *at < m) ++at;
          ring.insert(at, m);
        } else if (!on) {
          ring.erase(std::remove(ring.begin(), ring.end(), m), ring.end());
          if (ring.empty()) return;   // never nothing: value_allowed refuses this too
        }
        set("video.layout_cycle", layout_cycle_string(ring));
        return;
      }
      if (std::strcmp(key, "video.layout") == 0) pending_mode = value;
      cfg.set(key, value);
      apply(key, value);
      // Remembered where the player asked. The per-game file is only offered
      // when there is a game, and it is the file the layout hotkeys already
      // write, so the two agree about where a preference lives.
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      if (!ds::sdl::Config::store(path, key, value))
        std::fprintf(stderr, "settings: cannot write %s\n", path.c_str());
    }

    // What a change asked for and commit() will do when the menu closes.
    bool reopen_wanted = false, layout_wanted = false, fullscreen_wanted = false;
    std::function<void()> relayout;
    std::function<void()> refullscreen;
    void commit() override {
      // The layout first: reopening carries the live layout across, so doing
      // it the other way round would open the window on the old one and then
      // lay it out again.
      if (layout_wanted) { layout_wanted = false; relayout(); }
      if (fullscreen_wanted) { fullscreen_wanted = false; refullscreen(); }
      if (reopen_wanted) { reopen_wanted = false; reopen(); }
    }

    bool has_game() const override { return !game_ini.empty(); }

    // --- Controls ---------------------------------------------------------
    // The rows are the twelve DS buttons and then the hotkey actions, in the
    // order the config file lists them, so the page and the file read the
    // same way down.
    ds::sdl::Input& input;
    std::function<void()> reconfigure_input;

    // What a slot of a hotkey holds, defaults included; "none" for unset.
    std::string hot_value(bool pad, int a, int slot) const {
      const std::string key = (pad ? "padhotkeys." : "hotkeys.") + std::string(ds::sdl::action_name(static_cast<ds::sdl::Action>(a)))
                            + ds::sdl::Input::hot_suffix(slot);
      // The second binding has no default: unset is what it means.
      if (slot) return cfg.str(key, "none");
      return cfg.str(key, pad ? ds::sdl::Input::pad_hot_default(a) : ds::sdl::Input::key_hot_default(a));
    }
    static bool is_set(const std::string& v) { return !v.empty() && v != "none"; }

    // A hotkey's second row is only worth a line on the page once there is
    // something to put on it: it is shown when the player has set one, and
    // when the first slot is bound and a second could usefully join it. An
    // action bound to nothing in this column has no use for a second control,
    // and on the pad column that is most of the list.
    bool alt_row_shown(bool pad, int a) const {
      return is_set(hot_value(pad, a, 1)) || is_set(hot_value(pad, a, 0));
    }

    // The rows of a column, in order: the twelve DS buttons, then each hotkey
    // with its second row if that is shown, then the extras. Counting and
    // indexing go through the same rules, so a hidden row cannot leave the two
    // disagreeing.
    int binding_count(bool pad) const override {
      int n = ds::sdl::Input::button_count() + extra_count(pad);
      for (int a = 0; a < ds::sdl::Input::action_count(); ++a) n += alt_row_shown(pad, a) ? 2 : 1;
      return n;
    }

    // What an extra row holds, and how it reads.
    std::string extra_value(const Extra& e, bool pad) const {
      const char* key = extra_key(e, pad);
      const std::string v = cfg.str(key, extra_default(key, pad, cfg));
      // A stick, not an axis: that row names the stick the pen follows, or
      // the one that works the face buttons.
      if (extra_is_axis_remap(key)) return axis_remap_label(v);
      if (extra_is_stick(key))   // true/false: stick_dpad's old spellings (Input::parse_stick)
        return v == "left" || v == "true" || v == "1" ? "LEFT STICK" : v == "right" ? "RIGHT STICK" : "NONE";
      return pad ? ds::sdl::Input::pad_label(v) : upper(v);
    }

    Binding binding(bool pad, int i) const override {
      const int nb = ds::sdl::Input::button_count();
      Binding out;
      if (i < 0) return out;
      if (i < nb) {
        const char* name = ds::sdl::Input::button_name(i);
        out.key = (pad ? "pad." : "keys.") + std::string(name);
        out.label = upper(name);
        out.value = cfg.str(out.key, pad ? ds::sdl::Input::pad_default(i) : ds::sdl::Input::key_default(i));
      } else {
        int k = i - nb, a = 0, slot = 0;
        for (; a < ds::sdl::Input::action_count(); ++a) {
          const int rows = alt_row_shown(pad, a) ? 2 : 1;
          if (k < rows) { slot = k; break; }
          k -= rows;
        }
        if (a >= ds::sdl::Input::action_count()) {
          if (k >= extra_count(pad)) return out;   // past the end: no row
          const Extra& e = extra_at(pad, k);
          out.key = extra_key(e, pad);
          out.label = e.label;
          out.value = extra_value(e, pad);
          return out;                              // already in the page's own terms
        }
        const char* name = ds::sdl::action_name(static_cast<ds::sdl::Action>(a));
        out.key = (pad ? "padhotkeys." : "hotkeys.") + std::string(name) + ds::sdl::Input::hot_suffix(slot);
        out.label = upper(name) + (slot ? " (2)" : "");
        out.value = hot_value(pad, a, slot);
      }
      // The file writes SDL's names in lower case; the page reads better in
      // the font it has, which has no lower case anyway. Pad values get the
      // position pips and the L1/L2/SELECT spellings on top of that -- display
      // only, the file keeps SDL's names (input.h, pad_label).
      out.value = pad ? ds::sdl::Input::pad_label(out.value) : upper(out.value);
      return out;
    }

    bool has_pad() const override { return input.has_pad(); }
    void begin_capture(bool pad) override { input.begin_capture(pad); }
    void cancel_capture() override { input.cancel_capture(); }
    bool capturing() const override { return input.capturing(); }
    std::string take_capture() override { return input.take_capture(); }

    void bind(const std::string& key, const std::string& value0) override {
      std::string value = value0;
      // The pen follows a stick, and so do the stick-as-buttons rows, so
      // these store which one rather than the axis the player happened to
      // push. Anything that is not a stick says nothing about that and is
      // left alone -- the row keeps what it had.
      // An axis row stores the physical axis that moved, before the remap it
      // is setting -- the captured name is the remapped one. A button says
      // nothing about an axis, and the row keeps what it had.
      if (extra_is_axis_remap(key.c_str()) && value != "none") {
        std::string raw = input.captured_raw_axis();
        if (raw.empty()) return;
        if (raw[0] == '+') raw = raw.substr(1);
        value = raw;
      }
      if (extra_is_stick(key.c_str()) && value != "none") {
        const char* stick = ds::sdl::Input::stylus_axis_of(value);
        if (!stick) return;
        value = stick;
      }
      // A modifier cannot be built on the modifier, and neither can the pen's
      // tap: both are matched by the control alone, so a "mod+" here would
      // simply never fire.
      if ((key == "hotkeys.modifier" || key == "padhotkeys.modifier" ||
           key == "pad.stylus_button" || key == "pad.stylus_button.alt" || key == "pad.stylus_dpad") &&
          value.compare(0, 4, "mod+") == 0)
        value = value.substr(4);
      cfg.set(key, value);
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      if (!ds::sdl::Config::store(path, key, value))
        std::fprintf(stderr, "settings: cannot write %s\n", path.c_str());
      // Re-read the lot rather than poking one binding: configure() is what
      // resolves the modifier, the stylus chords and the collision warnings,
      // and half-applying a change would leave those stale.
      reconfigure_input();
    }

    void reset_bindings(bool pad) override {
      const int nb = ds::sdl::Input::button_count();
      const std::string& path = per_game && !game_ini.empty() ? game_ini : global_ini;
      for (int i = 0; i < nb; ++i) {
        const std::string k = (pad ? "pad." : "keys.") + std::string(ds::sdl::Input::button_name(i));
        const char* v = pad ? ds::sdl::Input::pad_default(i) : ds::sdl::Input::key_default(i);
        cfg.set(k, v);
        ds::sdl::Config::store(path, k, v);
      }
      for (int a = 0; a < ds::sdl::Input::action_count(); ++a)
        for (int slot = 0; slot < ds::sdl::Input::HOT_SLOTS; ++slot) {
          const std::string k = (pad ? "padhotkeys." : "hotkeys.") + std::string(ds::sdl::action_name(static_cast<ds::sdl::Action>(a))) + ds::sdl::Input::hot_suffix(slot);
          const char* v = slot ? "none" : pad ? ds::sdl::Input::pad_hot_default(a) : ds::sdl::Input::key_hot_default(a);
          cfg.set(k, v);
          ds::sdl::Config::store(path, k, v);
        }
      // The extras are rows of this column too, so "defaults" has to mean all
      // of it: a modifier left somewhere odd is exactly the sort of thing the
      // player reaches for this key to undo.
      for (int j = 0; j < extra_count(pad); ++j) {
        const char* k = extra_key(extra_at(pad, j), pad);
        const char* v = extra_default(k, pad, cfg);
        cfg.set(k, v);
        ds::sdl::Config::store(path, k, v);
      }
      reconfigure_input();
    }

    std::vector<std::string> collisions() const override { return input.collisions(); }

    // --- DS Options -------------------------------------------------------
    // With a generated firmware these are [user] in the config file, which is
    // what the firmware is built from. With a real dump they are the dump's
    // own settings pages: read from the image, written back to it in memory,
    // and persisted to the sidecar beside the dump -- never into the dump
    // itself, which the player may not be able to regenerate.
    std::string fw_override;

    bool user_in_firmware() const { return !nds.firmware_synthetic; }

    const char* user_settings_note() const override {
      return user_in_firmware() ? "IN THE FIRMWARE" : nullptr;
    }

    static bool is_user_key(const char* key) { return std::strncmp(key, "user.", 5) == 0; }

    // Which field of the settings block a config key names.
    static bool user_field(const char* key, NDS::UserField& out) {
      const std::string k = key + 5;
      if (k == "nickname")       { out = NDS::UserField::Nickname; return true; }
      if (k == "message")        { out = NDS::UserField::Message; return true; }
      if (k == "colour")         { out = NDS::UserField::Colour; return true; }
      if (k == "birthday_month") { out = NDS::UserField::BirthdayMonth; return true; }
      if (k == "birthday_day")   { out = NDS::UserField::BirthdayDay; return true; }
      if (k == "language")       { out = NDS::UserField::Language; return true; }
      return false;
    }

    std::string user_get(const char* key) const {
      ds::bios::UserSettings u;
      if (!nds.read_user_settings(u)) return "";
      const std::string k = key + 5;
      if (k == "nickname")       return u.nickname;
      if (k == "message")        return u.message;
      if (k == "colour")         return std::to_string(u.favourite_colour);
      if (k == "birthday_month") return std::to_string(u.birthday_month);
      if (k == "birthday_day")   return std::to_string(u.birthday_day);
      if (k == "language")       return std::to_string(u.language);
      return "";
    }

    void user_set(const char* key, const std::string& v) {
      ds::bios::UserSettings u;
      if (!nds.read_user_settings(u)) return;
      NDS::UserField f;
      if (!user_field(key, f)) return;
      const std::string k = key + 5;
      if (k == "nickname")            u.nickname = v;
      else if (k == "message")        u.message = v;
      else if (k == "colour")         u.favourite_colour = static_cast<u8>(std::atoi(v.c_str()) & 15);
      else if (k == "birthday_month") u.birthday_month = static_cast<u8>(std::atoi(v.c_str()));
      else if (k == "birthday_day")   u.birthday_day = static_cast<u8>(std::atoi(v.c_str()));
      else if (k == "language")       u.language = static_cast<u8>(std::atoi(v.c_str()) & 7);
      if (!nds.write_user_settings(f, u)) return;
      // Written out now rather than at exit: this is a setting the player has
      // just changed, and a kill would otherwise lose it.
      std::string err;
      if (!fw_override.empty() && !nds.save_firmware_override(fw_override, err))
        std::fprintf(stderr, "firmware settings: %s: %s\n", fw_override.c_str(), err.c_str());
    }

    static std::string upper(const std::string& s) {
      std::string out = s;
      for (char& c : out) {
        if (c == '_') c = ' ';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      return out;
    }
    bool save_per_game() const override { return per_game && has_game(); }
    void set_save_per_game(bool on) override { per_game = on && has_game(); }

    // The tier questions. effects_at_source means the frontend is drawing at
    // DS resolution for a hardware scaler, where the grid, the seams and
    // bilinear have no panel pixels to work with; chunky still applies there,
    // but only as the mean, which is what the scaler can do.
    bool panel_effects() const { return !display.effects_at_source(); }

    bool value_allowed(const ds::sdl::Setting& s, const char* value) const override {
      // The last ticked layout cannot be unticked: the hotkeys need somewhere
      // to go, and an empty ring would mean "only the current layout", which
      // is not what unticking everything reads as.
      Disp::Mode m;
      if (cycle_key(s.key, m))
        return std::strcmp(value, "false") != 0 || !in_cycle(m) || vs.layout_cycle.size() > 1;
      if (std::strcmp(s.key, "video.chunky") != 0 || panel_effects()) return true;
      return !std::strcmp(value, "false") || !std::strcmp(value, "mean");
    }

    bool enabled(const ds::sdl::Setting& s) const override { return !*disabled_reason(s); }

    const char* disabled_reason(const ds::sdl::Setting& s) const override {
      const auto flag = [&](const char* k, bool def) { return cfg.flag(k, def); };
      const Disp::Layout& l = display.current_layout();
      // The mode the page is working towards, so picking PIP on the LAYOUT
      // row lights the pip rows before the menu has closed.
      const Disp::Mode mode = effective_mode();
      const bool pip = mode == Disp::Mode::Pip;
      const bool dominant = mode == Disp::Mode::DominantV || mode == Disp::Mode::DominantH;
      switch (s.depends) {
      case ds::sdl::Dep::None: return "";
      case ds::sdl::Dep::FrameskipMode:
        // Frameskip itself is forced to 0 for a session, so its mode is moot
        // there too -- and the config may still say otherwise, which is what
        // this test would read.
        if (net_live && *net_live) return "NOT DURING A NETWORK SESSION";
        return cfg.num("emu.frameskip", 0) > 0 ? "" : "ONLY WITH FRAMESKIP ON";
      case ds::sdl::Dep::PanelEffects:
        return panel_effects() ? "" : "THIS SCREEN SCALES IN HARDWARE";
      case ds::sdl::Dep::GridSeam:
        if (!panel_effects()) return "THIS SCREEN SCALES IN HARDWARE";
        return flag("video.linear", false) ? "BILINEAR IS ON" : "";
      case ds::sdl::Dep::Chunky:
        return flag("video.linear", false) ? "BILINEAR IS ON" : "";
      case ds::sdl::Dep::ChunkyCell:
        if (flag("video.linear", false)) return "BILINEAR IS ON";
        return cfg.str("video.chunky", "false") == "false" ? "ONLY WITH CHUNKY ON" : "";
      case ds::sdl::Dep::Windowed:
        // A tier that owns the panel is already filling it; there is no
        // window to make bigger.
        return display.scaling() && !display.window() ? "THIS SCREEN IS ALWAYS FULL" : "";
      case ds::sdl::Dep::OneWindow:
        // Two windows show one screen each; there is nothing to lay out.
        return vs.dual_window ? "NOT WITH TWO WINDOWS" : "";
      case ds::sdl::Dep::Pip:
        return pip ? "" : "PIP LAYOUT ONLY";
      case ds::sdl::Dep::PipTouchHold:
        if (!pip) return "PIP LAYOUT ONLY";
        return l.pip_alpha < 1.0 ? "" : "ONLY WHEN THE PIP FADES";
      case ds::sdl::Dep::Dominant:
        return dominant ? "" : "DOMINANT LAYOUTS ONLY";
      case ds::sdl::Dep::DominantThreshold:
        if (!dominant) return "DOMINANT LAYOUTS ONLY";
        return cfg.str("video.dominant_ratio", "auto") == "auto" ? "" : "ONLY WHEN THE RATIO IS AUTO";
      case ds::sdl::Dep::Net:
#if DSPERATE_NET
        return "";
#else
        return "THIS BUILD HAS NO NETWORKING";
#endif
      case ds::sdl::Dep::NetInternet:
        // Reads the config, not the session: the row is restart-only, so what
        // it hangs off is the value being edited above it, not what this run
        // happens to be doing.
        return cfg.str("net.mode", "off") == "internet" ? "" : "ONLY WITH NETWORK FEATURES ON INTERNET";
      case ds::sdl::Dep::NetSession:
        return (net_live && *net_live) ? "NOT DURING A NETWORK SESSION" : "";
      case ds::sdl::Dep::ShortcutsPath:
        return shortcuts_dir(cfg).empty() ? "NEEDS PATHS.DSI_SHORTCUTS OR PATHS.GAMES" : "";
      }
      return "";
    }
  };

  Host host(cfg, nds, display, vs, global_ini, session.game_ini, input);
  host.reconfigure_input = [&] { input.configure(cfg); };
  host.fw_override = fw_override;
  host.net_live = &net_live;
  host.reopen = [&] { reopen_display(); };
  // The whole layout in one go, re-read from the config: every row on the
  // Layout page is a field of it, so there is nothing per-key to do. The mode
  // is the LAYOUT row's choice if one was made, else the live one -- the
  // layout hotkey or a loaded state may have moved it since the file was read.
  host.relayout = [&] {
    if (dual_window) { host.pending_mode.clear(); return; }   // two windows, one screen each: nothing to lay out
    VideoSetup want;
    if (!parse_video(cfg, want)) { host.pending_mode.clear(); return; }
    want.layout.mode = host.effective_mode();
    host.pending_mode.clear();
    display.set_layout(want.layout);
    vs.layout = want.layout;
    pip_alpha = static_cast<int>(want.layout.pip_alpha * 255.0 + 0.5);
    apply_visibility();
    menu_dirty = true;
  };
  host.refullscreen = [&] {
    if (display.fullscreen() == cfg.flag("video.fullscreen", false)) return;
    display.toggle_fullscreen();
    if (dual_window) display2.toggle_fullscreen();
    menu_dirty = true;
  };
  // Starting or stopping a network session, assigned once the transports are
  // in scope below. Declared here because host.apply needs to call it: the
  // NETWORK FEATURES row is a live setting now, and this is what it does.
  std::function<void(const std::string&)> set_net_mode;
  // Push one changed key into the running machine. Anything not named here
  // either needs the display reopened (the picture settings, handled below)
  // or is only read at startup, and its row says so.
  host.apply = [&](const char* key, const std::string& v) {
    const auto is = [&](const char* k) { return std::strcmp(key, k) == 0; };
    const bool on = v == "1" || v == "true" || v == "yes" || v == "on";
    // The rows are greyed during a session (Dep::NetSession), so this is the
    // belt to that braces: a config file edited under the menu cannot bring
    // frameskip back mid-session either.
    if (is("emu.frameskip")) { if (!net_live) fs_limit = std::atoi(v.c_str()); return; }
    if (is("emu.frameskip_mode")) { fs_adaptive = v != "fixed"; return; }
    // ::ds::jit, not ds::jit: a local `jit` (the interpreter switch) shadows
    // the namespace in here. The pricing is baked in when a block is
    // translated, so the cache goes with it -- otherwise the change would
    // only reach code the game had not run yet.
    if (is("emu.cpu_tuning")) {
      const int mode = cpu_oc_mode(v);
      cpu_oc_cfg = mode;
      cpu_oc_applied = cpu_oc_wanted(mode);
#if DSPERATE_JIT
      ::ds::jit::set_cpu_oc(static_cast<::ds::jit::CpuOc>(cpu_oc_applied));
      ::ds::jit::flush_all();
#endif
      nds.gpu3d.set_geometry_worker(mode != 0 || cfg.flag("emu.timing_oc", false) || cfg.flag("emu.gx_worker", false));
      return;
    }
    if (is("emu.timing_oc")) { nds.gpu3d.set_timing_oc(on); nds.gpu3d.set_geometry_worker(on || cfg.flag("emu.gx_worker", false) || cpu_oc_mode(cfg.str("emu.cpu_tuning")) != 0); return; }
    if (is("emu.gx_worker")) { nds.gpu3d.set_geometry_worker(on || cfg.flag("emu.timing_oc", false) || cpu_oc_mode(cfg.str("emu.cpu_tuning")) != 0); return; }
    if (is("emu.fast_load")) { nds.io.set_cart_bulk(on); return; }
    if (is("emu.dsi_nand_shortcuts")) {
      shortcuts_note = sync_nand_shortcuts(on);   // shown by the frame loop's toast
      if (boot_firmware && nds.cart) games = enumerate_library(cfg);
      return;
    }
    // NETWORK FEATURES. Live, so a player can put the radio up for a trade
    // and take it down again without quitting: Pokemon's Union Room comes back
    // to the game afterwards, which is what makes this worth having (there is
    // no way back from PictoChat or Download Play on hardware either way).
    if (is("net.mode")) { if (set_net_mode) set_net_mode(v); return; }
    if (is("emu.speed")) { speed_pct = std::atoi(v.c_str()); apply_limiter(); return; }
    if (is("emu.limiter")) { limiter_mode = v; apply_limiter(); return; }
    if (is("audio.buffer_size")) { apply_audio_buffer(v); return; }
    if (is("audio.latency_frames")) { audio.set_buffer_ms(std::atoi(v.c_str()) * ds::sdl::Audio::FRAME_MS); return; }
    if (is("emu.ff_speed")) { ff_speed = std::atoi(v.c_str()); return; }
    if (is("emu.ff_skip")) { ff_skip = std::atoi(v.c_str()); return; }
    if (is("emu.autosave")) { autosave = on; return; }
    if (is("video.aa")) { nds.gpu3d.renderer().set_aa(on); return; }
    if (is("video.fps")) { fps_osd = on; if (on && !show_fps) { fps_mark = SDL_GetPerformanceCounter(); emu_ticks = draw_ticks = wait_ticks = 0; } return; }
    if (is("video.pip_touch_hold")) { pip_touch_hold = std::max(0, std::atoi(v.c_str())); return; }
    // Everything below moves the picture about, so it is only noted here and
    // done when the menu closes (Host::commit).
    if (is("video.fullscreen")) { host.fullscreen_wanted = true; return; }
    // The hotkey ring is read when a hotkey is pressed; nothing on screen moves.
    if (is("video.layout_cycle")) {
      std::vector<Disp::Mode> ring;
      if (parse_layout_cycle(v, ring) && !ring.empty()) layout_cycle = ring;
      return;
    }
    if (is("video.layout") || is("video.screen") || is("video.pip_corner") || is("video.pip_scale") ||
        is("video.pip_alpha") || is("video.dominant_ratio") || is("video.dominant_threshold")) {
      host.layout_wanted = true;
      return;
    }
    // The picture settings are baked into the scaler's tables when the display
    // opens, so applying them means opening it again.
    if (is("video.linear") || is("video.lcd_grid") || is("video.seam") ||
        is("video.chunky") || is("video.chunky_cell") || is("video.integer_scale"))
      host.reopen_wanted = true;
  };
  menu.set_settings_host(&host);
  auto set_paused = [&](bool p) {
    if (p == paused) return;
    paused = p;
    audio.pause(p);
    display.set_page(p);
    if (p) flush_save(); else { pacer.reset(); fs_debt_ms = 0; }
    VLOG("%s\n", p ? "paused" : "resumed");
  };
  // A toast: the framed, four-second notice drawn over the game. Not an
  // achievements feature, though that was the first thing to need one -- it
  // is also how local wireless tells the player a scan found nobody, which is
  // the kind of thing that otherwise only reaches a terminal the handheld
  // does not have. ds::sdl::draw_toast does the drawing.
  // The toast on screen, and what is waiting behind it. One at a time and
  // timed like the slot label, because two unlocks can land on the same frame
  // and stacking them would cover the game.
  struct Toast { const char* header = nullptr; std::string title, detail; u32 points = 0; int frames = 0; };
  std::deque<Toast> toast_queue;
  Toast toast_now;
  int toast_left = 0;
  ds::sdl::Rect toast_last{};          // what the canvas has to take back out
  constexpr int TOAST_FRAMES = 240;    // four seconds: long enough to read two lines
  constexpr int TOAST_INFO_FRAMES = 150;
  // A DSi state that loaded without an SD card (NDS::sd_card_note; the log
  // has the long form). Twice a toast's time: the game has lost its card.
  auto sd_note_toast = [&] {
    if (nds.sd_card_note.empty()) return;
    Toast t;
    t.header = "SD CARD";
    t.title = "LEFT OUT OF THE STATE";
    t.detail = nds.sd_card_note.find("without one") != std::string::npos ? "THE STATE HAD NO CARD" : "ITS FOLDER HAS CHANGED";
    t.frames = TOAST_FRAMES * 2;
    toast_queue.push_back(std::move(t));
  };
  sd_note_toast();   // the state autoloaded at startup, before there were toasts
  bool dsp_warned = false;   // the DSP warning below has been shown for this start

  // Advances the toast clock. Called once per frame from the same place the
  // other per-frame overlay state is stepped.
  auto toast_step = [&] {
    if (toast_left > 0) { --toast_left; return; }
    if (toast_queue.empty()) return;
    toast_now = std::move(toast_queue.front());
    toast_queue.pop_front();
    toast_left = toast_now.frames;
  };
  const auto toast_on = [&] { return toast_left > 0; };
  auto draw_toast_on = [&](const ds::sdl::Canvas& c) {
    ds::sdl::draw_toast(c, toast_now.header, toast_now.title.c_str(),
                        toast_now.detail.empty() ? nullptr : toast_now.detail.c_str(),
                        toast_now.points);
  };

#if DSPERATE_CHEEVOS
  // RetroAchievements, Casual mode (docs/retroachievements-scoping.md). Off
  // unless asked for, and every failure here is reported once and then ignored:
  // a device with no libcurl, no network, or no account still plays games.
  ds::cheevos::Client cheevos;
  const bool cheevos_on = cfg.flag("cheevos.enabled", false);
#endif
// Networking is not a part of the achievements feature, and used to be nested
// inside its guard: DSPERATE_CHEEVOS=OFF with DSPERATE_NET=ON would not
// compile, because `lan` was declared in a block that had been preprocessed
// away while the frame loop still used it.
#if DSPERATE_NET
  std::unique_ptr<ds::net::LanMp> lan;
  std::unique_ptr<ds::net::SlirpDriver> slirp;
  // Starting and stopping a session is one function, used for the mode the
  // command line and the config asked for at startup and for the menu's
  // NETWORK FEATURES row later. `mode` is a net.mode value, plus "join" for
  // --lan-join, which has an address a menu row has nowhere to put.
  //
  // It can run at any point in a run because the console's MAC is settled
  // before it boots, every run (see set_wifi_mac_suffix above): the trap this
  // used to walk into was two instances sharing one dump's MAC, and that is
  // now impossible whether or not a session was ever asked for.
  //
  // What a session takes away it gives back on the way out. The three speed
  // knobs, frameskip and the fast-forward toggle are remembered as the player
  // had them and restored here, because they never asked for them off -- the
  // session did, and the session is over. docs/wifi-scoping.md.
  // Set when a guest heard nobody: see begin_guest_scan below.
  bool guest_retry_armed = false, radio_was_on = false, scanning = false;
  bool knobs_held = false;
  std::string held_cpu_oc = "false", held_timing_oc = "false", held_fast_load = "false";   // the values, as emu.cpu_tuning has three
  int held_speed = 100; std::string held_limiter = "auto";
  int  held_fs_limit = 0;
  bool held_ff_toggle = false;
  auto take_away_for_session = [&] {
    if (knobs_held) return;
    knobs_held = true;
    held_cpu_oc = cpu_oc_mode(cfg.str("emu.cpu_tuning")) == 2 ? "underclock" : cpu_oc_mode(cfg.str("emu.cpu_tuning")) ? "overclock" : "false";
    held_timing_oc = cfg.flag("emu.timing_oc", false) ? "true" : "false";
    held_fast_load = cfg.flag("emu.fast_load", false) ? "true" : "false";
    held_fs_limit = fs_limit;
    held_ff_toggle = ff_toggle;
    held_speed = speed_pct;
    held_limiter = limiter_mode;
    // A console in a session runs at the console's rate, full stop: the peer
    // (or the server) is keeping time, and a limiter of 30 or 144 -- or any
    // speed but 100 % -- is this machine deciding to disagree with it.
    if (speed_pct != 100 || limiter_mode != "auto") {
      if (speed_pct != 100 || !limiter_off)
        std::fprintf(stderr, "net: the game runs at the console's own rate for this session -- the network keeps time\n");
      speed_pct = 100;
      limiter_mode = "auto";
      apply_limiter();
    }
    if (fs_limit > 0) {
      std::fprintf(stderr, "net: frameskip is off for this session -- the emulator cannot set its own pace while the network keeps time\n");
      fs_limit = 0;
    }
    if (ff_toggle) {
      std::fprintf(stderr, "net: fast forward is off for this session\n");
      ff_toggle = false;
    }
    for (const char* k : {"emu.cpu_tuning", "emu.timing_oc", "emu.fast_load"})
      if (cpu_oc_mode(cfg.str(k)) != 0) {   // "on" for the two booleans, either tier for cpu_tuning
        std::fprintf(stderr, "net: %s is off for this session -- the network keeps time, so the machine cannot fake it\n", k);
        cfg.set(k, "false");       // in memory, not in the player's file
        host.apply(k, "false");    // and into the running machine
      }
  };
  auto give_back_after_session = [&] {
    if (!knobs_held) return;
    knobs_held = false;
    fs_limit = held_fs_limit;
    ff_toggle = held_ff_toggle;
    if (speed_pct != held_speed || limiter_mode != held_limiter) {
      speed_pct = held_speed;
      limiter_mode = held_limiter;
      apply_limiter();
      std::fprintf(stderr, "net: the frame limiter is back where it was -- the session is over\n");
    }
    const std::pair<const char*, const std::string*> knobs[] = {
      {"emu.cpu_tuning", &held_cpu_oc}, {"emu.timing_oc", &held_timing_oc}, {"emu.fast_load", &held_fast_load}};
    for (const auto& [k, held] : knobs)
      if (*held != "false") {
        std::fprintf(stderr, "net: %s is back on -- the session is over\n", k);
        cfg.set(k, *held);
        host.apply(k, *held);
      }
  };
  set_net_mode = [&](const std::string& mode) {
    // Down first, whatever is up. A guest says goodbye (LanMp::end_session
    // sends the disconnect) rather than just going silent, so the host drops
    // it from its player list instead of waiting out its slot.
    if (lan) { lan->end_session(); nds.io.wifi.set_transport(nullptr); lan.reset(); }
    if (slirp) { nds.io.set_net_driver(nullptr); slirp->stop(); slirp.reset(); }
    const bool was_live = net_live;
    net_live = false;

    if (mode == "internet") {
      // Wiimmfi's resolver, 178.62.43.212: it answers Nintendo's own hostnames
      // (nas.nintendowifi.net and the rest) with its own servers, which is the
      // whole mechanism by which a DS still reaches a matchmaking service.
      // Verified live 2026-09-12. Not a constant the player is stuck with --
      // wifi.dns takes any address, and this has moved before.
      constexpr ds::u32 kWiimmfiDns = 0xB23E2BD4;   // 178.62.43.212
      auto dns = ds::net::SlirpDriver::Dns::Custom;
      ds::u32 dns_addr = kWiimmfiDns;
      const std::string where = dns_arg ? dns_arg : cfg.str("wifi.dns", "wiimmfi");
      if (where == "host") { dns = ds::net::SlirpDriver::Dns::Host; dns_addr = 0; }
      else if (where != "wiimmfi") {
        in_addr parsed{};
        if (inet_pton(AF_INET, where.c_str(), &parsed) == 1) {
          dns_addr = ntohl(parsed.s_addr);
        } else {
          std::fprintf(stderr, "wifi.dns: \"%s\" is not host, wiimmfi or an address; using wiimmfi\n", where.c_str());
        }
      }
      // The internet through the emulated access point. Nothing to discover
      // and no peer to wait for: the driver is attached, and from there the
      // game associates with the AP and DHCPs its way on by itself.
      slirp = std::make_unique<ds::net::SlirpDriver>();
      if (!slirp->start(dns, dns_addr)) {
        std::fprintf(stderr, "internet: %s\n", slirp->error().c_str());
        slirp.reset();
      } else {
        nds.io.set_net_driver(slirp.get());
        net_live = true;
        VLOG("internet: up, DNS %s\n", where.c_str());
      }
    } else if (mode == "auto" || mode == "guest" || mode == "host" || mode == "join") {
      lan = std::make_unique<ds::net::LanMp>();
      bool up = lan->ok();
      if (up && (mode == "auto" || mode == "guest")) {
        // The same scan either way; auto may fall back to hosting when it
        // hears nobody, a guest may not (LanMp::start_auto).
        const auto role = lan->start_auto(lan_name, 2500, 16, mode == "auto");
        up = role != ds::net::LanMp::Role::None;
        if (up) VLOG("netplay: %s\n", role == ds::net::LanMp::Role::Host ? "no session heard, hosting" : ("joined " + lan->peer_name()).c_str());
      } else if (up) {
        up = mode == "join" ? lan->start_client(lan_name, lan_join)
                            : lan->start_host(lan_host ? lan_host : lan_name, 16);
      }
      if (!up) {
        std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
        lan.reset();
        // A guest heard nobody. Try once more when the game asks for its
        // radio, which is the better moment (see begin_guest_scan below).
        if (mode == "guest") guest_retry_armed = true;
      } else {
        VLOG("lan: %s, player %d\n", lan->is_host() ? "hosting" : "joined", lan->my_id());
        nds.io.wifi.set_transport(lan.get());
        net_live = true;
      }
    }

    if (net_live) take_away_for_session();
    else give_back_after_session();
    menu.set_network_session(net_live);
    // The pause menu is a stop with no session and an overlay with one, so a
    // change while it is open changes what it is. Nothing to do if the menu is
    // not up: raising it later reads net_live then.
    if (menu.open() && net_live != was_live) {
      if (net_live) { set_paused(false); display.set_page(true); }
      else set_paused(true);
      menu_dirty = true;
    }
  };
  // A guest that heard nobody gets one more go, timed far better than the
  // first: when the game first puts a frame on the air. At startup the player
  // has not reached the game's multiplayer menu yet and the other console
  // probably has not either, whereas a transmitted frame means they just
  // walked into the Union Room -- which is about when the person they are
  // trading with does the same. Once only, and only for a guest: auto hosts
  // when it hears nothing, so it has nothing to retry.
  //
  // The signal is Wifi::tx_frames(), NOT the radio having power: the firmware
  // powers the Wi-Fi block during its own boot, so keying on that fired the
  // scan while the console was still starting and put the notice on screen
  // before the player had gone anywhere.
  //
  // The scan is spread across frames rather than blocking like start_auto: it
  // now happens while a game is running, and stopping the machine for two and
  // a half seconds mid-Union-Room would break the audio and, under a session,
  // be the desync itself.
  Uint32 scan_began_ms = 0;
  constexpr Uint32 kScanMs = 2500;   // as start_auto: a host beacons once a second
  // Keep each line to about 24 characters: toast_box caps the panel at two
  // thirds of the DS screen and truncates past it, which on a 256-pixel
  // screen is roughly that. "NOTHING IS HOSTING ON THIS NETWORK" came out as
  // "NOTHING IS HOSTING ON TH..." the first time.
  auto net_toast = [&](const char* title, const char* detail, int frames) {
    Toast t;
    t.header = "LOCAL WIRELESS";
    t.title = title;
    if (detail) t.detail = detail;
    t.frames = frames;
    toast_queue.push_back(std::move(t));
  };
  auto begin_guest_scan = [&] {
    guest_retry_armed = false;
    if (!lan) lan = std::make_unique<ds::net::LanMp>();
    if (!lan->ok() || !lan->start_discovery()) {
      std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
      lan.reset();
      net_toast("SCAN FAILED", "THE NETWORK REFUSED IT", TOAST_FRAMES);
      return;
    }
    scanning = true;
    scan_began_ms = SDL_GetTicks();
    VLOG("lan: the game is on the air; looking for a session\n");
    net_toast("LOOKING FOR A SESSION", nullptr, static_cast<int>(kScanMs) * 60 / 1000);
  };
  auto finish_guest_scan = [&] {
    scanning = false;
    if (!lan) return;
    if (!lan->scan_join(lan_name)) {
      std::fprintf(stderr, "lan: %s\n", lan->error().c_str());
      lan.reset();
      // The one thing a player cannot find out any other way: a handheld has
      // no terminal, and until now this only ever reached stderr.
      net_toast("NO SESSION FOUND", "NOBODY IS HOSTING HERE", TOAST_FRAMES);
      return;
    }
    nds.io.wifi.set_transport(lan.get());
    net_live = true;
    take_away_for_session();
    menu.set_network_session(true);
    VLOG("lan: joined %s as player %d\n", lan->peer_name().c_str(), lan->my_id());
    // The name goes on the detail line: a session name is as long as its
    // host's nickname makes it, and truncation there costs nothing.
    net_toast("JOINED A SESSION", lan->peer_name().c_str(), TOAST_INFO_FRAMES);
  };
  {
    // What the flags and net.mode asked for, in the words set_net_mode takes.
    const char* mode = lan_join ? "join" : lan_host ? "host" : netplay ? "auto"
                     : lan_guest ? "guest" : internet ? "internet" : "off";
    if (std::strcmp(mode, "off") != 0) set_net_mode(mode);
  }
#else
  if (lan_host || lan_join || netplay || lan_guest) std::fprintf(stderr, "lan: built without DSPERATE_NET\n");
  if (internet) std::fprintf(stderr, "internet: built without DSPERATE_NET\n");
#endif
#if DSPERATE_CHEEVOS
  std::string cheevos_hash;      // the running game's identity; empty if it could not be hashed
  bool cheevos_set_asked = false;
  // What the game in the machine is, to RetroAchievements: the cart's hash,
  // or for a DSiWare title run from the NAND the one taken when it was read.
  // The loader cart is not a game and gets none, so nothing is asked for
  // until the picker has started one (launch_game calls this again).
  auto cheevos_identify = [&](const std::string& name) {
    cheevos_hash.clear();
    cheevos_set_asked = false;
    std::string err;
    if (!nds.cart) { cheevos_hash = dsi_title_cheevos_hash; return; }
    if (!ds::cheevos::rom_hash(nds.cart->source(), name, cheevos_hash, err, nds.dsi))
      std::fprintf(stderr, "cheevos: cannot identify this ROM: %s\n", err.c_str());
  };

  auto cheevos_show = [&] {
    for (const ds::cheevos::Message& m : cheevos.take_messages()) {
      // stderr as well as the screen: the log is where a problem gets
      // diagnosed, and a toast is gone in four seconds.
      std::fprintf(stderr, "cheevos: %s%s%s\n", m.text.c_str(),
                   m.detail.empty() ? "" : " -- ", m.detail.c_str());
      // A picture of the moment it unlocked, through the same shot_pending the
      // hotkey uses -- so it lands in the screenshots directory named like any
      // other. Usually that is this frame, the one the achievement triggered
      // on. The exception is a frame being skipped: `present` was decided
      // above, before the unlock was known, so the shot slips to the next
      // frame, where shot_pending forces a present. One frame late beats a
      // missed shot, and it is the same path the hotkey takes.
      //
      // It captures the game, not the toast: the toast goes on the canvas
      // afterwards, and a picture of the notice is less interesting than one
      // of what earned it.
      if (m.kind == ds::cheevos::Message::Kind::Unlock && cfg.flag("cheevos.auto_screenshot", false))
        shot_pending = true;
      if (!cfg.flag("cheevos.toasts", true)) continue;
      // cfg, not the menu host: both read the same key, and the host writes
      // through cfg, so a switch flipped in the menu is live on the next
      // message without any wiring between them.
      Toast t;
      t.header = m.kind == ds::cheevos::Message::Kind::Unlock ? "ACHIEVEMENT UNLOCKED"
               : m.kind == ds::cheevos::Message::Kind::Problem ? "RETROACHIEVEMENTS" : nullptr;
      t.title = m.text;
      t.detail = m.detail;
      t.points = m.points;
      t.frames = m.kind == ds::cheevos::Message::Kind::Unlock ? TOAST_FRAMES : TOAST_INFO_FRAMES;
      toast_queue.push_back(std::move(t));
    }
  };
  // What the Achievements pages read. The list is cached rather than rebuilt
  // per frame: rc_client_create_achievement_list allocates, and the menu asks
  // for rows on every idle tick it is drawn on.
  struct CheevosMenuHost final : ds::sdl::CheevosHost {
    ds::cheevos::Client* c = nullptr;
    std::vector<ds::cheevos::Client::Achievement> rows;
    ds::cheevos::Client::Summary sum{};
    void refresh() {
      if (!c) return;
      rows = c->achievements();
      sum = c->summary();
    }
    std::string status() const override {
      if (!c) return "NOT AVAILABLE IN THIS BUILD";
      switch (c->state()) {
      case ds::cheevos::State::Off:
        return c->unavailable_reason().empty() ? "TURNED OFF" : c->unavailable_reason();
      case ds::cheevos::State::SignedOut:   return "NOT SIGNED IN";
      case ds::cheevos::State::SigningIn:   return "SIGNING IN...";
      case ds::cheevos::State::SignedIn:    return "SIGNED IN AS " + c->username();
      case ds::cheevos::State::LoadingGame: return "LOADING ACHIEVEMENTS...";
      case ds::cheevos::State::Playing:     return "SIGNED IN AS " + c->username();
      // The dump is recognised; the game simply has no set yet. Said plainly
      // so nobody goes looking for a list that does not exist.
      case ds::cheevos::State::EmptySet:
        return "NO ACHIEVEMENTS PUBLISHED FOR THIS GAME YET";
      case ds::cheevos::State::NoSet:
        // The hash is the actionable part: RetroAchievements identifies a dump,
        // so a ROM from your own cart often is not one it knows even when the
        // game has a set. With the hash the player can ask for theirs to be
        // added; without it this line is a dead end.
        return "NO ACHIEVEMENTS FOR THIS ROM - HASH " + c->game_hash();
      }
      return "";
    }
    std::string progress() const override {
      if (!c || sum.total == 0) return {};
      char buf[128];
      // Encore is worth saying out loud: it is why an achievement the player
      // has already earned unlocks again, which otherwise looks like a fault.
      std::snprintf(buf, sizeof buf, "%u/%u EARNED  %u/%u POINTS%s",
                    sum.unlocked, sum.total, sum.points_earned, sum.points,
                    c->encore() ? "  (ENCORE)" : "");
      return buf;
    }
    bool signed_in() const override {
      if (!c) return false;
      const auto st = c->state();
      return st == ds::cheevos::State::SignedIn || st == ds::cheevos::State::Playing ||
             st == ds::cheevos::State::LoadingGame || st == ds::cheevos::State::NoSet ||
             st == ds::cheevos::State::EmptySet;
    }
    bool has_set() const override { return !rows.empty(); }
    int row_count() const override { return static_cast<int>(rows.size()); }
    Row row(int i) const override {
      Row r;
      if (i < 0 || i >= static_cast<int>(rows.size())) return r;
      const auto& a = rows[static_cast<size_t>(i)];
      r.title = a.title;
      // Measured progress where the set provides it ("12/50"), the description
      // otherwise: for a locked achievement the number is the useful half.
      r.detail = (!a.unlocked && !a.progress.empty()) ? a.progress + "   " + a.description
                                                     : a.description;
      r.points = a.points;
      r.unlocked = a.unlocked;
      r.unsupported = a.unsupported;
      return r;
    }
    void sign_in(const std::string& user, const std::string& password) override {
      if (c) c->sign_in(user, password);
    }

    // The switches go through the ordinary settings host, so they are written
    // to whichever file the player chose (global or per-game) and applied the
    // same way every other setting is. Nothing new to persist.
    ds::sdl::SettingsHost* settings = nullptr;
    static const char* key_of(Option o) {
      switch (o) {
      case Option::Toasts:     return "cheevos.toasts";
      case Option::Screenshot: return "cheevos.auto_screenshot";
      case Option::Encore:     return "cheevos.encore";
      }
      return "";
    }
    bool option(Option o) const override {
      if (!settings) return false;
      const std::string v = settings->get(key_of(o));
      // Only toasts default on; the other two are opt-in.
      if (v.empty()) return o == Option::Toasts;
      return v == "true" || v == "1" || v == "on";
    }
    void set_option(Option o, bool on) override {
      if (!settings) return;
      settings->set(key_of(o), on ? "true" : "false");
      // Encore is read when a game loads, so tell the session now and it takes
      // effect at the next load rather than being lost.
      if (o == Option::Encore && c) c->set_encore(on);
    }
    void sign_out() override {
      if (c) c->sign_out();
      rows.clear();
      sum = {};
    }
  } cheevos_menu;
  // Achievement progress in and out of save states.
  struct CheevosState final : CheevosStateHook {
    ds::cheevos::Client* c = nullptr;
    bool save(u32& game_id, std::vector<u8>& blob) override {
      if (!c) return false;
      game_id = c->game_id();
      return c->serialize_progress(blob);
    }
  } cheevos_state;

  // Puts a state's achievement progress in once there is a set to put it into.
  // Called before rc_client_do_frame, so the runtime is never evaluated for a
  // frame against progress belonging to a machine state it has left behind.
  auto cheevos_apply_state = [&] {
    if (!g_cheevos_pending.waiting) return;
    if (cheevos.state() == ds::cheevos::State::LoadingGame ||
        cheevos.state() == ds::cheevos::State::SigningIn) return;   // not yet; keep waiting
    if (cheevos.state() != ds::cheevos::State::Playing) {
      // No set will arrive for this game (signed out, or it has none). Nothing
      // to restore into, so stop holding the blob.
      g_cheevos_pending = {};
      return;
    }
    // The game id has to match. A state from another game cannot restore into
    // this set, and applying it anyway is how a false unlock happens -- so a
    // mismatch resets rather than guesses, as does a blob rcheevos rejects,
    // which it does when the set has changed since the state was written.
    const bool ok = !g_cheevos_pending.blob.empty() && g_cheevos_pending.game_id != 0 &&
                    g_cheevos_pending.game_id == cheevos.game_id() &&
                    cheevos.deserialize_progress(g_cheevos_pending.blob.data(),
                                                 g_cheevos_pending.blob.size());
    if (!ok) cheevos.reset();
    VLOG("cheevos: state progress %s\n", ok ? "restored" : "reset (none carried, or it did not apply)");
    g_cheevos_pending = {};
  };

  if (cheevos_on) {
    cheevos_menu.c = &cheevos;
    cheevos_menu.settings = &host;
    menu.set_cheevos_host(&cheevos_menu);
    cheevos_state.c = &cheevos;
    g_cheevos_state = &cheevos_state;
  }

  // The set can only be asked for once signed in, and signing in is
  // asynchronous -- so this watches for the moment rather than trying at boot
  // and reporting "Login required", which would read as a bug rather than as
  // "you are not signed in yet". It is also the hook phase 4's menu needs:
  // signing in there loads the set for the game already running.
  // What the account page was last drawn against. The menu only composites when
  // something says it changed, and the session changes on its own -- a sign-in
  // completing, a set arriving -- so without this the page sits on
  // "SIGNING IN..." until the player presses something.
  ds::cheevos::State cheevos_drawn_state = ds::cheevos::State::Off;
  auto cheevos_menu_follow = [&] {
    if (cheevos.state() == cheevos_drawn_state) return;
    cheevos_drawn_state = cheevos.state();
    cheevos_menu.refresh();   // the list and the counts move with it
    menu_dirty = true;
  };

  auto cheevos_catch_up = [&] {
    if (cheevos_set_asked || cheevos_hash.empty()) return;
    if (cheevos.state() != ds::cheevos::State::SignedIn) return;
    cheevos_set_asked = true;
    cheevos.load_game(nds, cheevos_hash);
  };
  if (cheevos_on) {
    std::string err;
    if (!cheevos.start(err)) {
      std::fprintf(stderr, "cheevos: %s\n", cheevos.unavailable_reason().c_str());
    } else {
      // Encore before anything loads: rcheevos evaluates it at game load and
      // ignores it afterwards.
      cheevos.set_encore(cfg.flag("cheevos.encore", false));
      // Sign in from the stored token. A password is never kept, so there is
      // nothing else to try here; a token the server has expired comes back as
      // a failure and the player signs in again from the menu (phase 4).
      ds::cheevos::Credentials creds;
      // A token file named on the command line comes first: it is this run's
      // explicit instruction, and the point of it is to override whatever is
      // stored. Nothing is written back -- the file is not ours, and the run
      // is as good as its token.
      const std::string token_file = cfg.str("cheevos.token_file");
      if (!token_file.empty()) {
        if (!ds::cheevos::read_token_file(token_file, creds, err)) {
          std::fprintf(stderr, "cheevos: %s\n", err.c_str());
        } else {
          // PPSSPP's file holds the token alone, so the account name has to
          // come from the config.
          if (creds.username.empty()) creds.username = cfg.str("cheevos.username");
          if (creds.username.empty()) {
            std::fprintf(stderr, "cheevos: %s has a token but no username; pass --cheevos-user or set cheevos.username\n",
                         token_file.c_str());
            creds = ds::cheevos::Credentials{};
          } else {
            std::fprintf(stderr, "cheevos: using the token from %s\n", token_file.c_str());
          }
        }
      }
      if (creds.empty() &&
          !ds::cheevos::load_credentials(ds::sdl::Config::dir(), creds, err) && !err.empty())
        std::fprintf(stderr, "cheevos: %s\n", err.c_str());
      // Failing that, the sign-in the CFW's own front end already did: on
      // ROCKNIX, EmulationStation writes a token to system.cfg, and a player
      // who signed in there should not have to type a password again on a
      // device with no keyboard. Only the token is read, never the password
      // those files also hold (see cheevos_client.h), and the file is never
      // written. Ours wins when we have one, so signing in from our own menu
      // (phase 4) takes over from then on.
      if (creds.empty() && cfg.flag("cheevos.use_system_login", true)) {
        std::string from;
        if (ds::cheevos::import_cfw_credentials(creds, from))
          std::fprintf(stderr, "cheevos: using the sign-in from %s\n", from.c_str());
      }
      if (!creds.empty()) cheevos.sign_in_with_token(creds.username, creds.token);
      else std::fprintf(stderr, "cheevos: not signed in\n");

      // The game's identity, computed now and used when the sign-in lands.
      // Hashing the cart's own source is safe *because* read_unpatched reads
      // past the secure-area rewrite Cart has already done by this point --
      // see docs/retroachievements-scoping.md.
      if (!launcher) cheevos_identify(session.rom_path);
    }
    cheevos_show();
  }
#endif
  // One tick of the pause menu: the timing it needs to repeat a held
  // direction and scroll a long name, then whatever the player chose.
  // Called from the idle loop while the machine is stopped, and once a
  // frame while it is not -- under a network session the menu is an
  // overlay over a running game, because the session cannot be stopped.
  // Starting a game from the loader cart's picker (and DS_LAUNCH_AT, which
  // does the same from a script).
  auto launch_game = [&](const std::string& pick) {
    VLOG("launcher: %s\n", pick.c_str());
    // DSiWare runs on the DSi machine with the launcher hand-off and a
    // made-up NAND, as `dsperate --dsi-mode game` does. There is no way
    // back to the DS menu from it: the session ends when the title does.
    const bool pick_shortcut = ds::io::is_shortcut_name(pick);
    const bool pick_dsi = pick_shortcut || ds::io::file_is_dsiware(pick);
    if (pick_shortcut && shortcut_nand.empty()) {
      std::fprintf(stderr, "launcher: %s needs the NAND (paths.dsi_nand in %s)\n", pick.c_str(), global_ini.c_str());
      return;
    }
    bool left_dsi = false;                      // a DS pick leaving the DSi machine
    if (pick_dsi && !have_dsi_bios()) {
      std::fprintf(stderr, "launcher: %s is DSiWare and needs the DSi BIOS pair (paths.bios9i/paths.bios7i in %s)\n", pick.c_str(), global_ini.c_str());
      return;
    }
    // All four dumps, for the reason the startup gate gives: a DS session on
    // FreeBIOS can reach this picker, and a DSiWare title started from it
    // would wedge rather than run.
    if (pick_dsi && !nds.bios_native) {
      std::fprintf(stderr, "launcher: %s is DSiWare and needs the DS BIOS dumps too (paths.bios9/paths.bios7 in %s); the built-in FreeBIOS cannot boot a DSi\n",
                   pick.c_str(), global_ini.c_str());
      return;
    }
    flush_save();
#if DSPERATE_JIT
    // Back off the firmware's strict timing: the game wants the speed,
    // and the environment override still wins if it was asked for.
    if (jit) { ds::jit::set_strict(std::getenv("DS_JIT_STRICT") != nullptr); ds::jit::flush_all(); }
#endif
    if (pick_dsi) {
      std::string err;
      if (!nds.load_dsi_bios(dsi_bios9i, dsi_bios7i, &err) || !nds.bios_native_dsi) {
        std::fprintf(stderr, "launcher: dsi bios: %s\n", err.empty() ? "not found" : err.c_str());
        return;
      }
      if (!open_dsi_sd()) return;
      // paths.dsi_firmware replaces the DS firmware this session booted with,
      // and the DS firmware's settings sidecar is left alone from here on.
      if (have_dsi_fw && !cli.has("paths.firmware") && fw != dsi_fw) {
        if (!nds.load_bios(bios9, bios7, dsi_fw, user, &err)) {
          std::fprintf(stderr, "launcher: dsi firmware: %s\n", err.c_str());
          input.request_quit();   // the DS firmware is gone: nothing to go back to
          return;
        }
        nds.set_wifi_mac_suffix(static_cast<u32>(cfg.num("net.mac_suffix", 0)));
        fw_override.clear();
        host.fw_override.clear();
        VLOG("launcher: DSi firmware %s\n", dsi_fw.c_str());
      }
      nds.dsi_nand.close();
      if (pick_shortcut && !nds.load_dsi_nand(shortcut_nand, &err)) {
        std::fprintf(stderr, "launcher: dsi nand: %s\n", err.c_str());
        input.request_quit();
        return;
      }
      nds.dsi_nand_synthetic = false;
      nds.dsi_nand_boot = false;   // picked from the DSi launcher: the made-up NAND is handed over, not booted
      nds.dsi_boot_blobs.clear();
      nds.dsi_loader_watch = false;
      nds.set_dsi(true);
      nds.dsi_hle_launch = true;
      dsi_mode = dsi_hle = true;
    } else if (nds.dsi) {
      // A DS game picked from the loader cart on the DSi launcher: a DS it
      // runs on, as the DS menu's picks do. The DSi firmware stays loaded; a
      // DSi runs DS games with it too.
      nds.dsi_nand.close();
      nds.dsi_sd.close();   // synced by the flush_save() above; a DS has no SD slot
      nds.dsi_nand_synthetic = false;
      nds.dsi_nand_boot = false;
      nds.dsi_boot_blobs.clear();
      nds.dsi_loader_watch = false;
      nds.dsi_font_hle = false;
      nds.set_dsi(false);
      dsi_mode = dsi_hle = false;
      left_dsi = true;
    }
    nds.reset();
    discard_session_cache();
    bool loaded;
    if (pick_shortcut) {
      loaded = load_shortcut_title(pick);
    } else if (pick_dsi && pick.size() > 4 && (pick.compare(pick.size() - 4, 4, ".cia") == 0 || pick.compare(pick.size() - 4, 4, ".CIA") == 0)) {
      std::vector<ds::u8> srl;
      std::string err;
      loaded = ds::io::read_dsiware(pick, srl, &err) && nds.load_rom_image(std::move(srl));
      if (!loaded) std::fprintf(stderr, "launcher: %s\n", err.c_str());
    } else {
      loaded = load_rom_notice(pick);
    }
    if (loaded && pick_dsi && !prepare_dsiware()) { input.request_quit(); return; }   // the DS machine is gone: nothing to go back to
    if (!loaded && pick_dsi) { input.request_quit(); return; }
    if (!loaded) {
      // Stay on the list rather than reset into nothing: another game
      // in the same directory may well be readable, and a cancelled
      // unpacking is a change of mind, not an error.
      std::fprintf(stderr, "launcher: could not read %s\n", pick.c_str());
      return;
    }
    nds.setup_direct_boot();
    if (left_dsi) VLOG("launcher: %s on a DS (%s)\n", pick.c_str(), jit ? "recompiler" : "interpreter");
    if (pick_dsi) {
      dsi_paths = pick_shortcut ? ds::io::NandPersistPaths::beside(shortcut_nand, cfg.str("paths.saves")) : hle_paths(pick);
      import_dsiware_saves();
      nand_writes_seen = nand_exported = nds.dsi_nand.writes;
      std::fprintf(stderr, "launcher: %s on the DSi (EXPERIMENTAL)\n", pick.c_str());
    }
    // Everything keyed to the ROM follows it, or the game would go on
    // writing the loader's saves, states, screenshots and cheats under
    // the loader's game code. --save is deliberately not carried over:
    // it pins one file, and it was given for the ROM on the command
    // line, not for whatever the player picks here.
    launcher = false;
    session.open(nds, cfg, pick, nullptr);
#if DSPERATE_CHEEVOS
    // The picked game is a different identity from the loader: drop any set
    // for what was running and ask again once identified (cheevos_catch_up).
    if (cheevos_on) { cheevos.unload_game(); cheevos_identify(pick); }
#endif
    menu.set_cheats(&nds.cheats.codes, &session.cheats.groups);
    session.load_enabled(nds);
    load_save(nds, session.sav);
    // The game's own auto slot, now that its code is known: a game
    // started from the picker resumes exactly as one named on the
    // command line does. After the battery save, so the state's SRAM
    // wins, and never during a replay or a recording.
    if (const std::string a = log.reading() || log.writing()
                                  ? std::string()
                                  : autoload_path(nds, session.states_dir, cfg.flag("emu.autoload", false));
        !a.empty() && load_state_file(nds, a, loaded_layout, got_layout)) {
      std::fprintf(stderr, "state: autoloaded %s\n", a.c_str());
      apply_loaded_layout();
      sd_note_toast();
    }
    sram_writes_seen = nds.cart ? nds.cart->sram_writes() : 0;
    state_slot = 0;
    refresh_slots();
    menu.set_open(false);
    set_paused(false);
  };
  auto pump_menu = [&] {
      if (!menu.open()) return;
      // The menu is ticked every idle pass, not only when a button moves:
      // holding a direction repeats, and a cheat name too long for its row
      // scrolls, both of which need to know how much time has gone by.
      const Uint32 now_ms = SDL_GetTicks();
      const u32 elapsed = static_cast<u32>(now_ms - menu_ms);
      menu_ms = now_ms;
      // A rebinding in progress repaints every tick: the page is showing
      // "PRESS ANY..." and has to come back to the new value the moment the
      // menu collects it (Menu::handle_controls).
      if (host.capturing()) menu_dirty = true;
      switch (menu.update(input.take_menu_presses(), input.menu_held(), elapsed)) {
      case Menu::Result::None: break;
      case Menu::Result::Resume:
        state_slot = menu.slot();
        if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
        menu.set_open(false);
        display.set_page(false);
        set_paused(false);
        break;
      // Both carry the same guards as the save-state hotkeys: a replay must
      // stay the run it recorded, and a recording is the inputs from boot,
      // which a load would leave unreplayable.
      case Menu::Result::Save:
        // The rows are hidden during a session, so this is unreachable
        // there; it is here because the guard belongs with the operation,
        // not with whatever happens to be able to reach it today.
        if (net_live) std::fprintf(stderr, "state: not during a network session\n");
        else if (save_readonly) std::fprintf(stderr, "state: not during a replay\n");
        else if (save_state_file(nds, state_path(nds, session.states_dir, menu.slot()), display.current_layout())) {
          g_state_refused.clear();   // the slot now holds a state this build made
          flush_save();
        }
        refresh_slots();
        break;
      case Menu::Result::Load:
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        // A load replaces the picture the menu is drawn over, so it also
        // leaves the menu: the player wants to see where they landed.
        if (load_state_file(nds, state_path(nds, session.states_dir, menu.slot()), loaded_layout, got_layout)) {
          apply_loaded_layout();
          sd_note_toast();
          state_slot = menu.slot();
          menu.set_open(false);
          set_paused(false);
          audio.clear();
          pacer.reset();
          fs_debt_ms = 0;
          flush_save();
        } else refresh_slots();
        break;
      case Menu::Result::Launch: launch_game(menu.chosen()); break;
      case Menu::Result::Quit: input.request_quit(); break;
      }
  };
  while (!input.quit() && !g_signalled && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e, display, dual_window ? &display2 : nullptr);
    for (ds::sdl::Action a : input.take_actions()) {
      using A = ds::sdl::Action;
      switch (a) {
      case A::Pause:
        // `paused` is false for the whole of a network session's menu visit,
        // so the test is "is the menu up", not "is the machine stopped".
        if (paused || menu.open()) {
          state_slot = menu.slot();
          if (menu.cheats_dirty()) { session.save_enabled(nds); menu.clear_cheats_dirty(); }
          menu.set_open(false);
          display.set_page(false);
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
      case A::SlotNext: state_slot = (state_slot + 1) % 10; show_slot(std::to_string(state_slot)); VLOG("state slot %d\n", state_slot); break;
      case A::SlotPrev: state_slot = (state_slot + 9) % 10; show_slot(std::to_string(state_slot)); VLOG("state slot %d\n", state_slot); break;
      case A::SaveState:
        // A state is a machine frozen mid-conversation: its peer, or the
        // server, is not frozen with it, and nothing in a state file restores
        // an ENet session or an open socket. docs/wifi-scoping.md.
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        if (save_state_file(nds, state_path(nds, session.states_dir, state_slot), display.current_layout())) {
          flush_save();   // the .sav and the state never diverge
          show_slot("STATE " + std::to_string(state_slot) + " SAVED");
        }
        break;
      case A::LoadState:
        if (net_live) { std::fprintf(stderr, "state: not during a network session\n"); break; }
        if (save_readonly) { std::fprintf(stderr, "state: not during a replay\n"); break; }
        // A recording is the inputs from boot; a load would leave it unreplayable.
        if (log.writing()) { std::fprintf(stderr, "state: not while recording\n"); break; }
        if (load_state_file(nds, state_path(nds, session.states_dir, state_slot), loaded_layout, got_layout)) {
          apply_loaded_layout();
          audio.clear();
          pacer.reset();
          fs_debt_ms = 0;
          flush_save();
          show_slot("STATE " + std::to_string(state_slot) + " LOADED");
          sd_note_toast();
        }
        break;
      // Without DS_FPS the measurement only runs while the counter is on, so
      // fps_mark is stale by however long it was off: restart the window, or
      // the first number shown would average over that whole gap.
      case A::FpsToggle:
        fps_osd = !fps_osd;
        if (fps_osd && !show_fps) { fps_mark = SDL_GetPerformanceCounter(); emu_ticks = draw_ticks = wait_ticks = 0; }
        break;
      case A::FastForwardToggle:
        if (net_live) { std::fprintf(stderr, "fast forward: not during a network session\n"); break; }
        ff_toggle = !ff_toggle; VLOG("fast forward %s\n", ff_toggle ? "on" : "off");
        break;
      default: break;
      }
    }
    if (paused) {
      // Nothing runs behind the menu, so it is composited only when something
      // about it changed -- otherwise this is a plain idle tick.
      pump_menu();
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
        // Where the panel's own pixels can be written, the menu is drawn on
        // them: it is then laid out for the screen it is actually on rather
        // than for one DS screen's slice of it, which is what a single-screen
        // device wants, and it comes out at panel resolution instead of being
        // upscaled with the picture. The other two tiers (the display engine,
        // whose scaler reads DS-sized buffers, and SDL_Renderer, which has no
        // frame buffer) keep the old path: the same drawing code over a
        // 256x192 scratch, which then goes through the scaler like a frame.
        const bool on_canvas = display.canvas_capable();
        if (!on_canvas) menu.draw(ds_canvas(menu_fb[menu_screen].data()));
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
          // The insets go down before the menu, so the PiP inset cannot land
          // on top of it.
          display.finish_views();
          if (dual_window) display2.finish_views();
          ds::sdl::Display::CanvasView cv;
          if (on_canvas && display.canvas(cv)) {
            menu.draw(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h});
            // The whole canvas: the page moves about as pages are walked, and
            // it is only presented when it changes, so there is nothing to be
            // gained by being precise about it.
            display.note_canvas_draw_all();
          }
          display.present();
          if (dual_window) display2.present();
          // No frame follows this present until the menu changes again, so a
          // tier that flips lazily (DrmOut queues behind a pending flip and
          // only issues it on the next begin_frame) must be pushed now, or
          // the menu shows up one press late.
          display.flush();
          if (dual_window) display2.flush();
          // Nothing may keep pointing into a buffer the display just released.
          set_scale_targets(target, false);
        } else {
          // The display-engine tier without chunky hands the core's
          // framebuffers to the layer as they are, so there is no scaled
          // frame -- but there is still the overlay layer, and the menu
          // belongs on it at panel resolution.
          ds::sdl::Display::CanvasView cv;
          if (on_canvas && display.canvas(cv)) {
            menu.draw(ds::sdl::Canvas{cv.px, cv.pitch, cv.w, cv.h});
            display.note_canvas_draw_all();
          }
          display.draw(fb);
          if (dual_window) display2.draw(fb);
        }
      }
#if DSPERATE_CHEEVOS
      // Paused: no frame to evaluate, but the session still has a queue to
      // work through (a pending unlock, a token refresh). idle() does that and
      // nothing else. ~60 ms here, comfortably inside the once-a-second the
      // session wants.
      if (cheevos_on) { cheevos.idle(); cheevos_catch_up(); cheevos_show(); cheevos_menu_follow(); }
#endif
      SDL_Delay(10);
      continue;
    }
    // The menu over a running game (a network session): it is ticked once a
    // frame here instead of in the idle loop, and it takes the buttons. The
    // machine still runs, so the game is handed a frame with nothing pressed
    // and no pen down -- the same as a player with their hands off the
    // console, which is what is actually happening.
    const bool menu_over_live = menu.open();
    if (menu_over_live) { pump_menu(); menu_dirty = menu_dirty || menu.dirty(); }
    input.update_stylus();
    ds::input::Frame in = input.frame();
    if (menu_over_live) {
      const bool lid_was = in.lid;
      in = ds::input::Frame{};
      in.lid = lid_was;   // the hinge is a state of the console, not an input
    }
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
    // The held hotkey is suppressed the same way the toggle is, but it is
    // read every frame, so it says so once rather than sixty times a second.
    bool fast = ff_toggle || input.fast_forward_held();
    if (net_live && fast) {
      static bool said = false;
      if (!said) { said = true; std::fprintf(stderr, "fast forward: not during a network session\n"); }
      fast = false;
    }
    if (fast != was_fast) { was_fast = fast; pacer.reset(); }
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
                           "           drop --no-frameskip-capture (or [emu] frameskip_capture = true) to skip them anyway\n");
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
      // The scanout tiers block here until a buffer is free: this wait is
      // where the display's pacing is felt, and it is outside every timed
      // series below, so it is counted on its own (DS_FPS "wait").
      const Uint64 tw = SDL_GetPerformanceCounter();
      scaled = display.begin_frame(target);
      if (dual_window) scaled = display2.begin_frame(target) && scaled;
      wait_ticks += SDL_GetPerformanceCounter() - tw;
    }
    set_scale_targets(target, scaled);
    fb_current = present && !scaled;   // a skipped frame renders nothing

    const Uint64 t0 = SDL_GetPerformanceCounter();
    // Everything since the last slice ended (present, buffer wait, pacing) is
    // the frontend's, not the frame's: the 3D shape controller subtracts it.
    static Uint64 last_slice_end = 0;
    if (last_slice_end) nds.gpu3d.note_external_ns(static_cast<u64>((t0 - last_slice_end) / ticks_per_ns));
#if DSPERATE_NET
    if (lan) lan->process();   // ENet and discovery, once per frame, on this thread
    // The stack's own timers and sockets. ap_recv pumps it too whenever the
    // game is listening, but a game between reads still has TCP to retransmit.
    if (slirp) slirp->process();
    // The guest's second look, driven across frames rather than blocking.
    // The edge is what matters: the game switching its radio on is the moment
    // it is about to go looking for someone, so that is when we look too.
    {
      const bool on_air = nds.io.wifi.tx_frames() > 0;
      if (on_air && !radio_was_on && guest_retry_armed && !scanning) begin_guest_scan();
      radio_was_on = on_air;
      if (scanning) {
        lan->scan_step();
        if (SDL_GetTicks() - scan_began_ms >= kScanMs) finish_guest_scan();
      }
    }
    // DS_WIFI_SLICE=1: spread the frame's emulation across its period in
    // 1 ms slices, ending just before the pacer's deadline, so a peer's CMD
    // is answered within a slice rather than after this frame's sleep. An
    // experiment, off by default: on the RG DS hosting PictoChat it halved
    // the host's longest reply wait (16 -> 8 ms) but left the audio queue
    // less margin (dips under one frame 37 vs 6 times in 3000), and the
    // plain pacer never let it run dry (docs/wifi-scoping.md, pacing).
    if (lan && !fast && std::getenv("DS_WIFI_SLICE")) {
      // pacer.next() is the deadline this frame was released at, i.e. its
      // start; the floor covers a frame that ran over and was released late.
      const Uint64 frame_end = std::max(pacer.next(), SDL_GetPerformanceCounter() - static_cast<Uint64>(frame_ns * ticks_per_ns / 2)) + static_cast<Uint64>(frame_ns * ticks_per_ns);
      constexpr u64 slice = ds::ARM9_CLOCK_HZ / 1000;
      constexpr int slices = static_cast<int>(ds::CYCLES_PER_FRAME / slice) + 1;
      for (int k = 1; !nds.run_frame_slice(slice); ++k) {
        const Uint64 due = frame_end - static_cast<Uint64>(frame_ns * ticks_per_ns * (slices - k) / slices);
        const Uint64 now = SDL_GetPerformanceCounter();
        if (due > now) { const double ms = (due - now) / (ticks_per_ns * 1e6); if (ms >= 0.5) SDL_Delay(static_cast<Uint32>(ms)); }
      }
    } else
#endif
    nds.run_frame();
#if DSPERATE_CHEEVOS
    // Exactly once per emulated frame, and only here. rcheevos keeps a delta
    // per memory reference, so a second call against the same frame collapses
    // delta onto current and single-frame edge triggers stop firing, silently
    // (tests/cheevos_memory_test.cpp pins this). This also drains the HTTP
    // completions, so every rcheevos callback runs on this thread.
    if (cheevos_on) {
      cheevos_apply_state();
      cheevos.frame();
      cheevos_catch_up();
      cheevos_show();
      cheevos_menu_follow();
    }
#endif
    // Every frame, cheevos or not: the toast is the frontend's, and local
    // wireless posts one too.
    toast_step();

    if (!shortcuts_note.empty()) {
      Toast t;
      t.header = "NAND SHORTCUTS";
      t.title = std::move(shortcuts_note);
      t.frames = TOAST_INFO_FRAMES;
      toast_queue.push_back(std::move(t));
      shortcuts_note.clear();
    }
    // A title started the DSP, which is not emulated (NDS::dsi_dsp_started):
    // say so on screen once per start, since it usually stops there.
    if (nds.dsi_dsp_started != dsp_warned) {
      dsp_warned = nds.dsi_dsp_started;
      if (dsp_warned) {
        std::fprintf(stderr, "dsi: this title uses the DSi's DSP, which DSperate does not emulate; it will likely not work\n");
        Toast t;
        t.header = "DSI DSP";
        t.title = "GAME LIKELY WON'T WORK";   // toast lines hold about 24 characters
        t.detail = "DSP IS NOT EMULATED";
        t.frames = TOAST_FRAMES * 2;
        toast_queue.push_back(std::move(t));
      }
    }

    // The console has switched itself off. On a firmware boot that is the
    // firmware leaving its settings pages -- the flash writes that saved them
    // have already landed in the image, so this is the moment to put them on
    // disk, and then to start the console again, which is what pressing the
    // power button would do next. A game reaching here is left running: it is
    // not expected to, and quitting on one stray write would lose more than
    // it saved.
#if DSPERATE_JIT
    // CPU tuning following the DSi Menu and the title it starts (cpu_tuning_for).
    if (jit && cpu_oc_cfg != 0 && cpu_oc_wanted(cpu_oc_cfg) != cpu_oc_applied) {
      cpu_oc_applied = cpu_oc_wanted(cpu_oc_cfg);
      VLOG("cpu tuning: %s (%s)\n", cpu_oc_applied == 2 ? "underclock" : cpu_oc_applied ? "overclock" : "off",
           !nds.dsi_title_running ? "the DSi Menu" : cpu_oc_applied ? "its title started" : "not for PictoChat or Download Play");
      ds::jit::set_cpu_oc(static_cast<ds::jit::CpuOc>(cpu_oc_applied));
      ds::jit::flush_all();
    }
#endif
    // A DSiWare title leaving (NDS::exit_requested): the session ends with it.
    if (nds.exit_requested) {
      std::fprintf(stderr, "dsi: the title has left (a soft reset); quitting\n");
      flush_save();
      input.request_quit();
      nds.exit_requested = false;
    }
    if (nds.power_off) {
      if (boot_firmware) {
        std::string err;
        if (fw_override.empty()) {
          // DSi mode: nothing is kept (see fw_override)
        } else if (nds.firmware_override_dirty() && !nds.save_firmware_override(fw_override, err))
          std::fprintf(stderr, "firmware settings: cannot save %s: %s\n", fw_override.c_str(), err.c_str());
        else if (nds.firmware_override_dirty())
          std::fprintf(stderr, "firmware settings: saved to %s\n", fw_override.c_str());
        std::fprintf(stderr, "power off: rebooting the firmware\n");
        autosave_now();       // the reset below discards the session
#if DSPERATE_JIT
        if (jit) ds::jit::flush_all();
#endif
        nds.reset();          // clears power_off, and re-seeds the clock
        if (nds.dsi) nds.setup_direct_boot();   // the DSi boots from its NAND again
        // A fresh firmware: the DS menu is back, so a card launch is a real
        // possibility again. Forget that this console ever did local wireless
        // (the picker's Download Play gate above).
        mp_ever = false;
      } else {
        nds.power_off = false;
      }
    }

    const Uint64 t1 = SDL_GetPerformanceCounter();
    last_slice_end = t1;
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
        else if (!dual_window && (cl.mode == Disp::Mode::DominantV || cl.mode == Disp::Mode::DominantH) && cl.primary == 0) ratio = display.dominant_ratio();
        if (ratio < 1.0) cursor_size *= std::clamp(static_cast<int>(1.0 / ratio + 0.5), 1, 4);
      }
      const bool slot_osd = slot_shown > 0;
      if (slot_shown > 0) --slot_shown;
      // The top-right field: the FPS counter when it is on, FF while fast
      // forward is, and both together ("FF 120") when both are.
      std::string fps_text;
      if (fast) fps_text = "FF";
      if (fps_osd) fps_text += (fast ? " " : "") + std::to_string(std::clamp(fps_value, 0, 999));
      const bool fps_field = !fps_text.empty();
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
      if (scaled) {
        // The crosshair first, and still in DS pixels: it points at a place on
        // the bottom screen, so it belongs in that view's coordinates wherever
        // the view is. It also has to go down before finish_views(), because
        // when the bottom screen is the PiP inset its target is the side
        // buffer that finish_views() then copies into the canvas.
        if (cursor) draw_cursor(CursorDst{target[1].px, target[1].pitch, target[1].h, target[1].xrun}, input.stylus_x(), input.stylus_y(), cursor_size);
        display.finish_views();
        if (dual_window) display2.finish_views();
        // Everything else is a message to the player rather than a part of the
        // picture, so it goes on the canvas, over the finished frame.
        ds::sdl::Display::CanvasView cv;
        if (display.canvas_capable() && display.canvas(cv)) {
          const ds::sdl::Canvas c{cv.px, cv.pitch, cv.w, cv.h};
          // Each of these says what it covered: the emulator repaints the
          // screens every frame but nothing repaints the letterbox, so the
          // display has to take the last overlay back out of a buffer before
          // it is used again (Display::note_canvas_draw).
          const auto note = [&](const ds::sdl::Rect& r) { display.note_canvas_draw(r.x, r.y, r.w, r.h); };
          if (slot_osd) note(draw_label(c, slot_text.c_str(), false));
          if (fps_field) note(draw_label(c, fps_text.c_str(), true));
          // The pause menu over a running game: the same draw the stopped
          // machine does, on the same canvas, only the picture underneath is
          // moving. Undimmed, deliberately -- the game is still going and the
          // player may well be watching it (a race finishing, a turn passing)
          // while they are in here.
          if (menu_over_live) { menu.draw(c); display.note_canvas_draw_all(); }
          // The toast covers more than a label, and nothing repaints the
          // letterbox, so the area it used has to be handed back even on the
          // frame it stops being drawn -- hence the note() outside the test.
          if (toast_on()) {
            draw_toast_on(c);
            toast_last = ds::sdl::toast_rect(c, toast_now.header, toast_now.title.c_str(),
                                             toast_now.detail.empty() ? nullptr : toast_now.detail.c_str(),
                                             toast_now.points);
          }
          if (toast_last.w) {
            note(toast_last);
            if (!toast_on()) toast_last = {};
          }
          if (flash_alpha) {
            draw_flash(c, flash_alpha);
            display.note_canvas_draw_all();
            ds::sdl::Display::CanvasView cv2;
            if (dual_window && display2.canvas(cv2)) {
              draw_flash(ds::sdl::Canvas{cv2.px, cv2.pitch, cv2.w, cv2.h}, flash_alpha);
              display2.note_canvas_draw_all();
            }
          }
        } else if (target[osd_screen].px) {
          // The display-engine tier: the targets are the DS-sized buffers its
          // scaler reads, so the canvas is that buffer and the label lands
          // where it did before.
          const auto tgt_canvas = [](const ds::sdl::Display::Target& t) {
            return ds::sdl::Canvas{t.px, t.pitch, t.xrun ? static_cast<int>(t.xrun[ds::SCREEN_W]) : static_cast<int>(ds::SCREEN_W), static_cast<int>(t.h)};
          };
          const ds::sdl::Canvas c = tgt_canvas(target[osd_screen]);
          if (slot_osd) draw_label(c, slot_text.c_str(), false);
          if (fps_field) draw_label(c, fps_text.c_str(), true);
          if (menu_over_live) menu.draw(c);
          if (toast_on()) draw_toast_on(c);
          if (flash_alpha) for (int i = 0; i < 2; ++i) if (target[i].px) draw_flash(tgt_canvas(target[i]), flash_alpha);
        }
        display.present();
        if (dual_window) display2.present();
      } else {
        const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
        if (cursor) {
          std::memcpy(cursor_fb.data(), fb[1], cursor_fb.size() * 4);
          draw_cursor(CursorDst{cursor_fb.data(), ds::SCREEN_W, ds::SCREEN_H, nullptr}, input.stylus_x(), input.stylus_y(), cursor_size);
          fb[1] = cursor_fb.data();
        }
        // The overlay layer, where there is one: the labels go on the panel's
        // own pixels instead of into a copy of a DS framebuffer, which is both
        // sharper and one less 192 K copy. The flash stays below -- it covers
        // every pixel, and on the overlay that would be a panel-sized upload
        // on each of its frames.
        ds::sdl::Display::CanvasView ocv;
        const bool want_osd = slot_osd || fps_field || toast_on() || menu_over_live;
        const bool osd_on_canvas = display.canvas_capable() && want_osd && display.canvas(ocv);
        if (osd_on_canvas) {
          const ds::sdl::Canvas c{ocv.px, ocv.pitch, ocv.w, ocv.h};
          if (slot_osd) draw_label(c, slot_text.c_str(), false);
          if (fps_field) draw_label(c, fps_text.c_str(), true);
          if (toast_on()) draw_toast_on(c);
          if (menu_over_live) menu.draw(c);
          display.note_canvas_draw_all();
        }
        // After the cursor: when the overlays are on the bottom screen this
        // copies the frame that already has the crosshair in it, so both show.
        if (!osd_on_canvas && want_osd) {
          std::memcpy(osd_fb.data(), fb[osd_screen], osd_fb.size() * 4);
          const ds::sdl::Canvas od = ds_canvas(osd_fb.data());
          if (slot_osd) draw_label(od, slot_text.c_str(), false);
          if (fps_field) draw_label(od, fps_text.c_str(), true);
          if (toast_on()) draw_toast_on(od);
          if (menu_over_live) menu.draw(od);
          fb[osd_screen] = osd_fb.data();
        }
        // Last, over whatever the cursor and the overlays left: a screen
        // still pointing at the GPU's own buffer is copied out first.
        if (flash_alpha) for (int i = 0; i < 2; ++i) {
          if (fb[i] == nds.gpu.framebuffer(i)) { std::memcpy(flash_fb[i].data(), fb[i], flash_fb[i].size() * 4); fb[i] = flash_fb[i].data(); }
          draw_flash(ds_canvas(const_cast<u32*>(fb[i])), flash_alpha);
        }
        display.draw(fb);
        if (dual_window) display2.draw(fb);
      }
    }
    // An unlimited limiter outruns the speakers the same way fast forward
    // does, so it sheds audio the same way: play the newest and stay in sync.
    audio.push(nds, fast || limiter_off);
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
      else {
        menu.set_slot(state_slot);
        refresh_slots();
#if DSPERATE_CHEEVOS
        if (cheevos_on) cheevos_menu.refresh();
#endif
        menu.set_open(true);
      }
      menu_dirty = true;
      menu_ms = SDL_GetTicks();
      // Under a network session the menu comes up over a running game. The
      // machine cannot stop: the peer (or the server) does not stop with it,
      // and a console that goes quiet for the length of a menu visit has left
      // the session. So the menu is opened and the emulator keeps running --
      // it takes the player's button presses (the game gets none while it is
      // up) and is drawn over the live frame instead of over a frozen one.
      //
      // display.set_page still goes on: it tells the display-engine tier a
      // text page is on screen, which is what keeps the glyphs legible.
      if (net_live) display.set_page(true);
      else set_paused(true);
    }
    // The loader cart's picker: the console's own launch fade is the signal.
    //
    // Launching the card drives both engines' MASTER_BRIGHT to white
    // (Gpu::screens_forced_white), where PictoChat, the Download Play menus,
    // the settings pages and the shutdown they end in do not, and the white
    // stretch of the firmware's own boot is white pixels rather than a forced
    // screen -- checked on all of them. So the register says "the card was
    // launched", with no tap and no rectangle of the menu's layout to keep up
    // to date: it catches the launch whether the player tapped the panel or
    // selected it with the D-pad and A, which a tap test cannot see at all.
    //
    // The one other thing that forces white is Download Play *booting its
    // downloaded program*, which the original measurements could not see
    // because Download Play could not get that far then. mp_ever below is
    // what tells the two apart.
    //
    // The cart read at the loader's arm9_rom_offset would be better still and
    // Cart::launch_read() still offers it, but this firmware never issues it:
    // the fade lands and the cart bus stays silent for ever after (measured --
    // the ROMCTRL access count is identical either side of the launch), so
    // nothing downstream of the fade can be waited on.
    //
    // Latched, because the white stays up: the picker would otherwise go
    // straight back up the moment the player closed it.
    if (nds.io.wifi.mp_active()) mp_ever = true;
    if (launcher) {
      // On the DSi launcher the fade also opens its boot splash and closes
      // Health & Safety, so there only a fade after the launcher hashed the
      // loader's header for its whitelist counts (NDS::dsi_loader_watch).
      const bool white = nds.gpu.screens_forced_white() && (!nds.dsi || nds.dsi_loader_launched);
      if (white && mp_ever) {
        // Download Play's child program, not a card launch. Raising the list
        // here would also pause the emulator, and the session's timing does
        // not survive that. Latched like the launch below, because the child's
        // white holds for as long as it likes and this would otherwise say so
        // once a frame for the rest of the boot.
        if (!launch_latched) {
          launch_latched = true;
          VLOG("launcher: forced white after a local-wireless session -- Download Play booting "
               "its program, not a card launch; the picker stays down\n");
        }
      } else if (white && !launch_latched) {
        launch_latched = true;
        launching = true;
        VLOG("launcher: the card was launched (frame %llu); raising the list\n", static_cast<unsigned long long>(frames));
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
      // DS_FRAME_SERIES=<path>: "emu work" ms per frame in run order, as the
      // headless frontend writes it -- for the shape of a tail, not its size.
      static FILE* series = [] { const char* p = std::getenv("DS_FRAME_SERIES"); return p ? std::fopen(p, "w") : nullptr; }();
      if (series) std::fprintf(series, "%.3f %.3f\n", frame_ms.back(), work_ms.back());
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
    // The automatic buffer size needs to know whether the machine is keeping
    // up, because depth answers a hitch and never a deficit -- growing into
    // one only pins the rate control at its clamp. Fast forward is behind by
    // design and says nothing about the machine, so it does not get a vote.
    if (audio.active() && !(fast && ff_speed <= 0))
      audio.auto_tick(static_cast<double>(t2 - t0) * ticks_to_ms <= frame_budget_ms);
    // DS_AUDIO_QUEUE=<path>: one line a frame --
    //
    //   depth   queue depth in frames of audio
    //   ppm     what the rate control is doing to the sample rate
    //   target  the depth it is steering towards
    //   dry     running count of frames that found the queue empty
    //   in_rate the SPU's output rate, which a DSi title can change
    //
    // The first two are the acceptance measurement for the rate control
    // (docs/frame-pacing-scoping.md): a steady depth and a trim that is not
    // sitting on its clamp. The rest are what an automatic buffer size has to
    // decide on, logged from the run that would have driven it.
    if (audio.active()) {
      static FILE* aq = [] { const char* p = std::getenv("DS_AUDIO_QUEUE"); return p ? std::fopen(p, "w") : nullptr; }();
      if (aq) std::fprintf(aq, "%.3f %.0f %.2f %llu %.1f\n", audio.queued_frames(), audio.rate_trim_ppm(), audio.target_frames(),
                           static_cast<unsigned long long>(audio.stats().dry), audio.input_rate());
    }
    ds::prof::frame_mark();   // marks the emu slice: the present is not in a stage, it lands in "untimed" of work_ms

    const Uint64 t3 = SDL_GetPerformanceCounter();
    // One clock, one number. base_scale is what the limiter and emu.speed
    // came to; fast forward, while it is held, asks for a floor under that
    // rather than a multiple of it -- ff_speed is a multiple of *real time*,
    // as its row has always said, so holding it at 2x on a 30 Hz limiter
    // gives 2x and holding it on a 240 Hz one does not slow the game down to
    // 2x. ff_speed 0 uncaps whatever the limiter says, which is the point of
    // keeping it: a limiter of 60 with a fast forward that goes as fast as
    // the machine will.
    const double scale = fast && ff_speed > 0 ? std::max(base_scale, static_cast<double>(ff_speed)) : base_scale;
    if ((fast && ff_speed <= 0) || limiter_off) pacer.reset();   // unthrottled: do not bank the time it gains
    else pacer.wait(scale);

    pace_ticks += SDL_GetPerformanceCounter() - t3;
    fps_pace_ticks += SDL_GetPerformanceCounter() - t3;

    ++frames;
    // DS_SHOT_AT=N: a screenshot (the hotkey's, into paths.screenshots) after
    // frame N -- for offscreen/replay runs, where no hotkey can fire.
    if (static const long shot_at = [] { const char* v = std::getenv("DS_SHOT_AT"); return v ? std::atol(v) : -1L; }(); shot_at >= 0 && frames == static_cast<u64>(shot_at)) shot_pending = true;
    // DS_LAUNCH_AT=<frame>:<path>: start that game at that frame as the
    // picker would (for scripted tests of the loader's launch path).
    if (static const char* la = std::getenv("DS_LAUNCH_AT"); la && std::strchr(la, ':') && frames == std::strtoull(la, nullptr, 10)) launch_game(std::strchr(la, ':') + 1);
    if (nds.cart && nds.cart->sram_dirty()) {
      if (nds.cart->sram_writes() != sram_writes_seen) { sram_writes_seen = nds.cart->sram_writes(); sram_quiet_since = frames; }
      else if (frames - sram_quiet_since >= 60) flush_save();
    }
    if (dsi_mode && nds.dsi_nand.writes != nand_exported) {
      if (nds.dsi_nand.writes != nand_writes_seen) { nand_writes_seen = nds.dsi_nand.writes; nand_quiet_since = frames; }
      else if (frames - nand_quiet_since >= 120) flush_dsi();
    }
    if (nds.dsi_sd.writes != sd_synced) {
      if (nds.dsi_sd.writes != sd_writes_seen) { sd_writes_seen = nds.dsi_sd.writes; sd_quiet_since = frames; }
      else if (frames - sd_quiet_since >= 120) flush_sd();
    }

    if ((show_fps || fps_osd) && frames % 60 == 0) {   // DS_FPS=1 and/or the on-screen counter
      const Uint64 now = SDL_GetPerformanceCounter();
      const double secs = static_cast<double>(now - fps_mark) / SDL_GetPerformanceFrequency();
      const double to_ms = 1e3 / SDL_GetPerformanceFrequency() / 60.0;
      const double fps = secs > 0 ? 60.0 / secs : 0.0;
      fps_value = fps >= 999.0 ? 999 : static_cast<int>(fps + 0.5);
      if (show_fps) {
        const double wall_ms = static_cast<double>(now - fps_mark) * to_ms;   // per frame, same unit as the rest
        // The SPU rate is live rather than a constant: a DSi title moves it to
        // 47605 Hz through SNDEXCNT, and the resampler ratio follows it, so a
        // statistics line that named 32768 would be naming the wrong input.
        std::fprintf(stderr, "%.1f fps (%.0f%%), emu %.1f ms, present %.1f ms, wait %.1f ms, pace %.1f ms (spin %.0f us), other %.1f ms, audio queued %.1f frames (%+.0f ppm, %.1f -> %u Hz)\n",
                     fps, 100.0 * fps / (ds::ARM9_CLOCK_HZ / double(ds::CYCLES_PER_FRAME)),
                     emu_ticks * to_ms, draw_ticks * to_ms, wait_ticks * to_ms, fps_pace_ticks * to_ms, pacer.spin_us(),
                     wall_ms - (emu_ticks + draw_ticks + wait_ticks + fps_pace_ticks) * to_ms, audio.queued_frames(), audio.rate_trim_ppm(),
                     audio.input_rate(), audio.rate());
        if (fs_limit > 0) std::fprintf(stderr, "  frameskip: %llu frames skipped (%s, limit %d)\n",
                                       static_cast<unsigned long long>(fs_skipped), fs_adaptive ? "adaptive" : "fixed", fs_limit);
      }
      fps_mark = now;
      emu_ticks = draw_ticks = wait_ticks = fps_pace_ticks = 0;
    }
  }

  autosave_now();
  flush_save();
  discard_session_cache();
  // Not after a DSiWare session: what a DSi title wrote to the flash is not a
  // DS menu setting to keep beside the DS firmware.
  if (!fw_override.empty() && nds.firmware_override_dirty() && !nds.dsi) {
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
#if DSPERATE_NET
  if (lan) {
    VLOG("lan: reply/host waits %u, total %.1f ms, max %.1f ms, timeouts %u\n", lan->wait_count(), lan->wait_total_ms(), lan->wait_max_ms(), lan->wait_timeouts());
  }
#endif
  // How the output buffer held up over the run. `dry` is the one that matters:
  // the queue was empty when a frame looked at it, so the device played
  // silence until the next push -- the gap a player hears, and the event an
  // automatic buffer size grows on. A run with drops but no dry frames was
  // fast-forwarding or had lost its device, which is not the same fault.
  if (const ds::sdl::Audio::Stats& as = audio.stats(); as.frames) {
    VLOG("audio: queue depth %.2f..%.2f frames over %llu, dry %llu, under 1 frame %llu, under 0.5 %llu, dropped %llu; device buffer %u samples (%.2f frames), ring overruns %llu\n",
         as.min_depth, as.max_depth, static_cast<unsigned long long>(as.frames), static_cast<unsigned long long>(as.dry),
         static_cast<unsigned long long>(as.under_one), static_cast<unsigned long long>(as.under_half),
         static_cast<unsigned long long>(as.dropped), audio.device_samples(), audio.device_buffer_frames(),
         static_cast<unsigned long long>(nds.spu.ring_overruns()));
  }
  // What staying under the real-time cap cost, if it was on: the frames it had
  // to act on out of the run, and the mean nap on those frames. A run that
  // never saturated a core reports nothing to do.
  if (pacer.rt_relief()) {
    unsigned relief_frames = 0;
    const double relief_us = pacer.relief_us(&relief_frames);
    VLOG("rt relief: %u of %llu frames gave time back, %.0f us each on those\n",
         relief_frames, static_cast<unsigned long long>(frames), relief_us);
  }
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
