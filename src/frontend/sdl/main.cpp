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
    "                  option below has a key there, and games/<CODE>.ini overrides per title)\n"
    "  --scale N       window scale (default 2)\n"
    "  --fullscreen    start fullscreen\n"
    "  --layout L      vertical (default) or horizontal: screens stacked or side by side\n"
    "  --dual-window   one window per video display, one DS screen each (dual-panel\n"
    "                  handhelds; also what direct scanout needs on them)\n"
    "  --linear        smooth scaling instead of nearest\n"
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
    else if (arg("--scale")) cli.set("video.scale", argv[++i]);
    else if (flag("--dual-window")) cli.set("video.dual_window", "true");
    else if (arg("--layout")) cli.set("video.layout", argv[++i]);
    else if (arg("--frames")) frame_limit = std::atol(argv[++i]);
    else if (arg("--record")) record = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--save")) save_arg = argv[++i];
    else if (flag("--fullscreen")) cli.set("video.fullscreen", "true");
    else if (flag("--linear")) cli.set("video.linear", "true");
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
  auto apply_cli = [&] { for (const char* k : {"paths.bios9", "paths.bios7", "paths.firmware", "video.scale", "video.dual_window", "video.layout",
                                              "video.fullscreen", "video.linear", "video.accel", "video.vsync", "audio.enabled", "audio.volume",
                                              "audio.mic", "emu.jit", "emu.quantum"}) if (cli.has(k)) cfg.set(k, cli.str(k)); };
  apply_cli();
  const std::string bios9 = cfg.str("paths.bios9"), bios7 = cfg.str("paths.bios7"), fw = cfg.str("paths.firmware");
  if (bios9.empty() || bios7.empty() || fw.empty()) { std::fprintf(stderr, "BIOS and firmware paths are needed (--bios9/--bios7/--firmware or [paths] in %s)\n", global_ini.c_str()); return 2; }

  NDS nds;
  if (!nds.load_bios(bios9.c_str(), bios7.c_str(), fw.c_str())) { std::fprintf(stderr, "could not load BIOS/firmware\n"); return 1; }
  nds.reset();
  if (!nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  // The per-game file goes on top of the global one, the command line on top of both.
  std::string game_ini;
  if (nds.cart) {
    game_ini = ds::sdl::Config::game_path(nds.cart->header().game_code);
    if (cfg.load(game_ini)) std::fprintf(stderr, "config: %s\n", game_ini.c_str());
    apply_cli();
    std::fprintf(stderr, "game: %.12s [%.4s]\n", nds.cart->header().game_title, nds.cart->header().game_code);
  }
  // Core knobs that the core reads from the environment.
  if (cfg.has("emu.idle_skip") && !std::getenv("DS_IDLE_SKIP")) setenv("DS_IDLE_SKIP", cfg.str("emu.idle_skip").c_str(), 1);

  int scale = cfg.num("video.scale", 2);
  if (scale < 1) scale = 1;
  const bool fullscreen = cfg.flag("video.fullscreen", false), linear = cfg.flag("video.linear", false), accel = cfg.flag("video.accel", false);
  bool audio_on = cfg.flag("audio.enabled", true), mic_on = cfg.flag("audio.mic", true);
  const bool jit = cfg.flag("emu.jit", true), vsync = cfg.flag("video.vsync", true), dual_window = cfg.flag("video.dual_window", false);
  const long quantum = cfg.num("emu.quantum", 0);   // event-bound interleave (DraStic's rule): 5-10 % faster than lockstep
  ds::sdl::Display::Layout layout = ds::sdl::Display::Layout::Vertical;
  {
    const std::string l = cfg.str("video.layout", "vertical");
    if (l == "horizontal") layout = ds::sdl::Display::Layout::Horizontal;
    else if (l != "vertical") { std::fprintf(stderr, "unknown layout %s\n", l.c_str()); return 2; }
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
  if (dual_window) {
    if (SDL_GetNumVideoDisplays() < 2) { std::fprintf(stderr, "--dual-window needs two video displays\n"); SDL_Quit(); return 1; }
    // Which display is the physical bottom panel depends on the driver, both
    // verified on the dual-panel board: under KMSDRM display 0 is DSI-1,
    // which is -- unintuitively -- the lower panel, while sway's canvas
    // arranges the outputs the other way around.
    const char* vd = SDL_GetCurrentVideoDriver();
    const int bottom_display = vd && !std::strcmp(vd, "KMSDRM") ? 0 : 1;
    if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout, accel, 0, 1 - bottom_display) ||
        !display2.open("DSperate (bottom)", scale, fullscreen, linear, vsync, layout, accel, 1, bottom_display)) { SDL_Quit(); return 1; }
    if (display.scaling() != display2.scaling()) { std::fprintf(stderr, "dual-window: mixed display modes\n"); SDL_Quit(); return 1; }
  } else if (!display.open("DSperate", scale, fullscreen, linear, vsync, layout, accel)) { SDL_Quit(); return 1; }

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
      case A::LayoutNext: {
        if (dual_window) break;
        const bool across = display.current_layout() != ds::sdl::Display::Layout::Horizontal;
        display.set_layout(across ? ds::sdl::Display::Layout::Horizontal : ds::sdl::Display::Layout::Vertical);
        if (!game_ini.empty()) ds::sdl::Config::store(game_ini, "video.layout", across ? "horizontal" : "vertical");
        break;
      }
      case A::Screenshot: screenshot(nds, states_dir, display.current_layout() == ds::sdl::Display::Layout::Horizontal); break;
      case A::Lid: input.set_lid(!input.lid()); std::fprintf(stderr, "lid: %s\n", input.lid() ? "closed" : "open"); if (input.lid()) flush_save(); break;
      case A::SlotNext: state_slot = (state_slot + 1) % 10; std::fprintf(stderr, "state slot %d\n", state_slot); break;
      case A::SlotPrev: state_slot = (state_slot + 9) % 10; std::fprintf(stderr, "state slot %d\n", state_slot); break;
      case A::SaveState: case A::LoadState: std::fprintf(stderr, "%s: not implemented yet\n", ds::sdl::action_name(a)); break;
      case A::FastForwardToggle: break;   // pacing: see below
      default: break;
      }
    }
    if (paused) { SDL_Delay(10); continue; }
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
    ds::sdl::Display::Target target[2];
    bool scaled = display.begin_frame(target);
    if (dual_window) scaled = display2.begin_frame(target) && scaled;
    for (int i = 0; i < 2; ++i)
      nds.gpu.set_scale_target(i, scaled ? ds::gpu::Gpu::ScaleTarget{target[i].px, target[i].pitch, target[i].h, target[i].xrun}
                                         : ds::gpu::Gpu::ScaleTarget{});

    const Uint64 t0 = SDL_GetPerformanceCounter();
    nds.run_frame();

    const Uint64 t1 = SDL_GetPerformanceCounter();
    if (scaled) {
      display.end_frame();
      if (dual_window) display2.end_frame();
    } else {
      const u32* fb[2] = {nds.gpu.framebuffer(0), nds.gpu.framebuffer(1)};
      display.draw(fb);
      if (dual_window) display2.draw(fb);
    }
    audio.push(nds);
    const Uint64 t2 = SDL_GetPerformanceCounter();
    emu_ticks += t1 - t0;
    draw_ticks += t2 - t1;
    draw_ticks_total += t2 - t1;
    frame_ms.push_back(static_cast<double>(t1 - t0) * ticks_to_ms);
    work_ms.push_back(static_cast<double>(t2 - t0) * ticks_to_ms);

    const Uint64 t3 = SDL_GetPerformanceCounter();
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
