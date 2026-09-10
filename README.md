# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Agentic coding is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

A Nintendo DS emulator that reimplements DraStic's JIT and NEON rendering
techniques, with modern niceties from melonDS such as per-screen windows.
Built for and measured on ARM handhelds (the Anbernic RG DS, an RK3566 with
four A55 cores); commercial games boot and play. Everything is verified against
melonDS with frame dumps and instruction traces.

DSperate is a **clean-room** reimplementation of DraStic's *techniques*,
documented by studying its freely-distributable debug-symbol build (approved
for distribution by its developer, Exophase) and measuring it on real
hardware. No DraStic code is in this tree. See [docs/techniques](docs/techniques).

## Features

### Core

- **Both CPUs, interpreted or recompiled.** ARM946E-S and ARM7TDMI
  interpreters on every host; an ARM → AArch64 recompiler and an ARM → ARMv7
  recompiler ([src/core/cpu/jit/README.md](src/core/cpu/jit/README.md)),
  verified instruction by instruction against the interpreter.
- **melonDS-grade cycle timing** with per-page cost tables and TCM windows,
  plus an idle-loop detector that proves a poll loop side-effect-free and skips
  to the next event.
- **Two interleave modes.** 128-cycle lockstep with melonDS (the default for
  measurement) or event-bound as DraStic does it (the default for play, a few
  percent faster).
- **Full system.** DMA with burst timing, timers, IPC, SPI (touchscreen,
  firmware flash, power management, microphone), RTC, divider/sqrt, and
  register-level Wi-Fi (no frames: PictoChat and Download Play do not work).
- **Retail cartridges.** KEY1/KEY2 protocol, direct boot, per-title save-chip
  database (EEPROM and FLASH variants), and IR-cart pass-through for the
  Pokémon titles that need it. The GBA slot is empty.
- **Zipped ROMs.** `.zip` files holding an `.nds` load directly (detected by
  magic, not extension), with an on-disk unpack cache. No zip64, 7z or rar.
- **2D.** Both engines complete: every BG mode, sprites, windows, mosaic,
  blending, extended palettes, display capture, display FIFO, master
  brightness. Rendered lazily in one batch per frame from a write journal,
  engine B on a worker thread, with NEON line kernels.
- **3D.** Full geometry engine (timed command FIFO, matrix stacks, lighting,
  clipping, tests) and a software rasteriser (textures, toon/highlight,
  shadows, fog, edge marking, anti-aliasing) that matches melonDS pixel for
  pixel. Runs asynchronously on three worker threads with a decoded-texture
  cache and a skip for resubmitted frames.
- **Sound.** All sixteen SPU channels (PCM8/16, ADPCM, PSG, noise), capture
  units, 32768 Hz output; the frontend paces the emulator from the audio queue.
- **No dumps required.** Without BIOS/firmware files, a built-in FreeBIOS and a
  generated firmware run games by direct boot. Real dumps (`bios9.bin`,
  `bios7.bin`, `firmware.bin`, never provided here) unlock the firmware menu and
  exact timing.

### Playing

- **Save states.** Ten slots plus a hidden auto slot that can be written on
  quit and resumed on the next launch. Exact round trip, includes the screen
  layout and achievement progress, refuses to load across a different BIOS.
- **Battery saves** written shortly after the game stops writing and again on
  pause, lid close and exit (SIGTERM included).
- **Pause menu** drawn over the game and driven with the DS buttons: save/load,
  slot list, cheats, options pages (emulation, visual effects, layout,
  controls, DS settings), achievements, resume and quit.
- **Action Replay cheats** from a `usrcheat.dat` database, matched by game
  code and header checksum, toggled per game from the pause menu.
- **RetroAchievements** (Casual mode only, no Hardcore) via rcheevos: sign in
  with a username or a token the CFW already holds, unlock toasts, an
  achievements list and account page in the pause menu, encore mode, and an
  optional screenshot on unlock. Uses the system's libcurl at runtime.
- **Firmware boot.** With no ROM the console boots its own DS menu with the
  clock set from the host. Settings changed inside it are kept in a sidecar
  file so the dump is never written to. A built-in loader cart in the slot
  raises a game picker from `[paths] games`; the chosen game is direct-booted.
- **Fast forward** (held or toggled, optionally capped) and **frameskip**
  (adaptive or fixed) that skips only drawing, so the emulation stays exact.
  Skipping runs in whole display periods for games that alternate screens.
- **Input recording and replay.** A played session becomes a deterministic,
  reproducible benchmark; [scenes/](scenes/) holds the ones used in the docs.
- **Microphone and lid.** Real ALSA capture (DC-blocked and noise-gated) or a
  held-key fake mic; a real hinge switch, host suspend, or a hotkey drives the
  lid.
- **Controls.** Keyboard and controller remapping, controller hotkeys as
  `mod+button` chords, a stick-driven stylus crosshair, and touch or mouse on
  the bottom screen.
- **Config.** An INI in `~/.config/dsperate/` with every key commented, per-game
  override files by ROM name or game code, and ready-made profiles in
  [configs/](configs/) (melonDS-like, DraStic-like, Knulli "Advanced DraStic").

### Display

- **Layouts:** vertical, horizontal, single, picture-in-picture, and two
  "dominant" modes where one screen takes the largest whole scale and the other
  fills what is left. Hotkeys cycle layouts and swap screens, remembered per game.
- **Scaling filters** applied per scanline straight into the presented buffer:
  nearest, bilinear, LCD grid, box-filter seams (sharp, shimmer-free), and
  chunky 2x2 cells; optional integer scaling (letterbox or crop).
- **Fastest path to the panel, chosen automatically:** the display engine's
  hardware scaler on Allwinner handhelds (Miyoo A30), fbdev on the H700 boards,
  zero-copy dmabuf under Wayland with direct scanout where allowed, our own page
  flips under KMSDRM, and SDL's renderer only where nothing else applies.
- **Dual-window mode** for dual-panel handhelds: one fullscreen window per
  display, one DS screen each.
- **On-screen overlays:** FPS counter, state slot number, screenshot flash,
  achievement toasts.

## Building

Requires CMake ≥ 3.16, Ninja, a C++17 compiler, and SDL2 for the playable
frontend. The only vendored third-party code is miniz's DEFLATE decompressor
and rcheevos.

    cmake --preset host && cmake --build --preset host && ctest --preset host

Cross builds: `aarch64-cross` (needs `aarch64-linux-gnu-g++`) and
`arm32-cross` (needs `arm-linux-gnueabihf-g++`); tests run under
`qemu-*-static` when present. The recompiler and NEON kernels build only on
AArch64 and ARMv7 hosts; elsewhere you get the interpreter and the portable
renderer.

CMake switches: `DSPERATE_JIT`, `DSPERATE_NEON`, `DSPERATE_TESTS`,
`DSPERATE_HEADLESS`, `DSPERATE_SDL`, `DSPERATE_WAYLAND`, `DSPERATE_CHEEVOS`,
`DSPERATE_HARDEN`, and `DSPERATE_PGO=generate|use` (a profile for AArch64 is
committed under [pgo/](pgo/)).

## Running

    dsperate [game.nds|game.zip] [--bios9 F --bios7 F --firmware F] [options]

`dsperate --help` lists every option; each has a key in the settings file and
the command line overrides it. Default keys: arrows and `X`/`Z`/`S`/`A`/`Q`/`W`
for the face and shoulder buttons, Enter/Right Shift for Start/Select, `P`
pause menu, `Tab` fast forward, `F5`/`F7` save/load state, `F2`/`F3` change
slot, `F4`/`F10` cycle layout, `F6` swap screens, `F9` screenshot, `L` lid,
`M` fake microphone, `F` fullscreen, `Escape` quit.

`dsperate-headless` is the measurement harness: no window or audio, boots the
firmware or a ROM with `--direct`, and offers instruction traces, frame and
audio dumps, save-state checkpoints and cheats from the command line. It never
writes a battery save back.

Performance knobs that trade accuracy for speed, off by default: `--cpu-oc`,
`--timing-oc`, `--fast-load`. Turn them off first if a game misbehaves.

## Diagnostics, tests and tools

Environment variables (`DS_VERBOSE`, `DS_FRAME_STATS`, `DS_FPS`, `DS_PROFILE`,
`DS_FRAME_HASH`, `DS_IO_CENSUS`, `DS_JIT_*`, `DS_R3D_THREADS` and others) are
documented where they are read. `ctest` runs the unit tests in
[tests/](tests/): interpreter, recompiler against interpreter, NEON kernels
against portable references, page table, scheduler, I/O, SPU, 2D/3D pipelines
and the texture cache. [tools/](tools/) holds the frame/trace/audio comparison
scripts, the exactness checks over the recorded scenes, the PGO refresh
scripts, and `mkcart.py` for building a custom loader cart.

## Licence

GPLv3 — see [LICENSE](LICENSE). melonDS (GPLv3) is used as reference and, with
attribution, as a code source. FreeBIOS is under its own licence in
[src/core/bios/LICENSE.freebios](src/core/bios/LICENSE.freebios).
