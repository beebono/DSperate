// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// SDL2 frontend: direct boot, both screens stacked, sound, and input from a
// keyboard, a game controller or a touchscreen. Deliberately minimal — no
// savestates, no configuration, no menus; battery saves are kept because the
// games expect them.
#include "core/nds.h"
#include "core/profile.h"
#include "core/input/input_log.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "audio.h"
#include "display.h"
#include "input.h"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace ds;

const char* kUsage =
    "usage: dsperate-sdl <rom.nds> --bios9 F --bios7 F --firmware F [options]\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --linear        smooth scaling instead of nearest\n"
    "  --no-audio      run without sound (frames are paced by the clock)\n"
    "  --no-vsync      present without waiting for the display refresh\n"
    "  --interp        interpreter instead of the recompiler\n"
    "  --frames N      quit after N frames (for repeatable measurements)\n"
    "  --record F      write the played inputs to F (one record per frame)\n"
    "  --replay F      play the inputs in F instead of the controls; quits at its end\n";

// Battery save file next to the ROM.
std::string save_path(const std::string& rom) {
  const size_t dot = rom.find_last_of('.');
  return (dot == std::string::npos ? rom : rom.substr(0, dot)) + ".sav";
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
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "save: cannot write %s\n", path.c_str()); return; }
  std::fwrite(nds.cart->sram().data(), 1, nds.cart->sram().size(), f);
  std::fclose(f);
  nds.cart->clear_sram_dirty();
}

} // namespace

int main(int argc, char** argv) {
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr;
  int scale = 2;
  long frame_limit = 0;
  const char *record = nullptr, *replay = nullptr;
  bool fullscreen = false, linear = false, audio_on = true, jit = true, vsync = true;

  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--scale")) scale = std::atoi(argv[++i]);
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (!std::strcmp(argv[i], "--fullscreen")) fullscreen = true;
    else if (!std::strcmp(argv[i], "--linear")) linear = true;
    else if (!std::strcmp(argv[i], "--no-audio")) audio_on = false;
    else if (!std::strcmp(argv[i], "--no-vsync")) vsync = false;
    else if (!std::strcmp(argv[i], "--interp")) jit = false;
    else if (!std::strcmp(argv[i], "--help")) { std::fputs(kUsage, stderr); return 0; }
    else rom = argv[i];
  }
  if (!rom || !bios9 || !bios7 || !fw) { std::fputs(kUsage, stderr); return 2; }
  if (scale < 1) scale = 1;

  NDS nds;
  if (!nds.load_bios(bios9, bios7, fw)) { std::fprintf(stderr, "could not load BIOS/firmware\n"); return 1; }
  nds.reset();
  if (!nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  nds.setup_direct_boot();
#if DSPERATE_JIT
  if (jit && !ds::jit::attach(nds, true, true)) return 1;
#else
  (void)jit;
#endif
  const std::string sav = save_path(rom);
  load_save(nds, sav);

  ds::input::Log log;
  if (record && replay) { std::fprintf(stderr, "--record and --replay are exclusive\n"); return 2; }
  if (record && !log.open_write(record)) { std::fprintf(stderr, "cannot write %s\n", record); return 1; }
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    std::fprintf(stderr, "replay: %u frames from %s\n", log.frames(), replay);
  }
  ds::prof::enabled = std::getenv("DS_PROFILE") != nullptr;

  u32 init = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
  if (audio_on) init |= SDL_INIT_AUDIO;
  if (SDL_Init(init) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }

  ds::sdl::Display display;
  if (!display.open("DSperate", scale, fullscreen, linear, vsync)) { SDL_Quit(); return 1; }

  ds::sdl::Audio audio;
  if (audio_on) audio.open();

  ds::sdl::Input input;
  input.open_controllers();

  // Wall-clock pacing when there is no audio queue to pace against.
  const double frame_ns = 1e9 * ds::CYCLES_PER_FRAME / ds::ARM9_CLOCK_HZ;
  Uint64 next_frame = SDL_GetPerformanceCounter();
  const double ticks_per_ns = static_cast<double>(SDL_GetPerformanceFrequency()) / 1e9;

  const bool show_fps = std::getenv("DS_FPS") != nullptr;
  Uint64 fps_mark = SDL_GetPerformanceCounter();
  Uint64 emu_ticks = 0, draw_ticks = 0;
  u64 frames = 0;
  while (!input.quit() && (frame_limit == 0 || frames < static_cast<u64>(frame_limit))) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle(e, display);
    ds::input::Frame in = input.frame();
    if (log.reading()) { if (!log.read(in)) break; }   // the controls still quit; the log ends the run
    else if (log.writing()) log.write(in);
    ds::input::apply(nds, in);

    const Uint64 t0 = SDL_GetPerformanceCounter();
    nds.run_frame();

    const Uint64 t1 = SDL_GetPerformanceCounter();
    const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
    display.draw(fb);
    audio.push(nds);
    const Uint64 t2 = SDL_GetPerformanceCounter();
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;

    if (audio.active()) {
      audio.pace();
    } else {
      next_frame += static_cast<Uint64>(frame_ns * ticks_per_ns);
      const Uint64 now = SDL_GetPerformanceCounter();
      if (next_frame > now) {
        const double wait_ms = (next_frame - now) / (ticks_per_ns * 1e6);
        if (wait_ms > 1.0) SDL_Delay(static_cast<Uint32>(wait_ms));
      } else {
        next_frame = now;      // running behind: do not build up a debt
      }
    }

    if (++frames % 300 == 0 && nds.cart && nds.cart->sram_dirty()) write_save(nds, sav);

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

  if (nds.cart && nds.cart->sram_dirty()) write_save(nds, sav);
  if (log.writing()) std::fprintf(stderr, "recorded %u frames to %s\n", log.frames(), record);
  log.close();
  ds::prof::report();
  input.close();
  audio.close();
  display.close();
  SDL_Quit();
  return 0;
}
