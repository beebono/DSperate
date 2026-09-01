# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Agentic coding is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

An attempt at a Nintendo DS emulator that reimplements DraStic JIT + NEON Rendering
techniques combined with modern niceties that melonDS provides like per-screen windows
and maybe overlaid shader support later on.

---

## Status

Commercial games boot and play. Everything below is in, verified against
melonDS (frame dumps, instruction traces) and measured on the Anbernic RG DS
(RK3566, four A55s) it is built for:

- **CPUs.** ARM946E-S and ARM7TDMI interpreters everywhere; on AArch64 an
  ARM → AArch64 recompiler for both ([src/core/cpu/jit/README.md](src/core/cpu/jit/README.md)),
  verified instruction by instruction against the interpreter. melonDS-grade
  cycle timing (per-page cost tables, TCM windows, ARM9 stores priced at bus
  cost), and an idle-loop detector that proves a poll loop side-effect-free
  and skips it to the next event.
- **System.** DMA (main-RAM burst timing, GXFIFO and display-FIFO modes),
  timers, IPC, SPI (touchscreen, firmware flash including page writes, power
  management and the microphone amplifier), RTC, the divider/sqrt unit,
  Wi-Fi register probing (no frames), and the two-CPU interleave either in
  128-cycle lockstep with melonDS or event-bound as DraStic does it.
- **Cartridge.** Retail Slot-1 carts: KEY1/KEY2 command protocol, direct boot,
  the save chip with melonDS's per-title save-type list (EEPROM and FLASH
  variants), and the IR carts' pass-through (Pokémon HG/SS, B/W, B2/W2).
  The GBA slot is an empty slot.
- **2D.** Both engines complete — every BG mode, sprites, windows, mosaic,
  blending, extended palettes, display capture, the main-memory display FIFO,
  master brightness — rendered lazily from a per-engine write journal in one
  batch per frame (exact by construction; a VRAM write trap catches the
  frame up when the picture could change under it), engine B on a worker
  thread, with NEON line kernels diffed against their portable references.
- **3D.** Geometry engine (command FIFO with cycle timing, matrix stacks,
  lighting, clipping, box/position/vector tests) and a software rasteriser
  with textures, toon/highlight shading, shadows, fog, edge marking and
  anti-aliasing that matches melonDS pixel for pixel. The raster runs
  asynchronously on three worker threads in horizontal bands, with a
  content-validated decoded-texture cache, batched span stages in NEON, and
  a skip for frames that resubmit the same geometry.
- **Sound.** The SPU mixes all sixteen channels (PCM8/16, ADPCM, PSG, noise),
  capture units and the output at 32768 Hz; the SDL frontend plays it and
  paces the emulator from the audio queue.
- **Sessions.** Battery saves, save states (ten slots, exact round trip),
  input recording and replay (a played scene becomes a benchmark), a real or
  fake microphone, and the lid/hinge.
- **Frontend.** SDL2: INI config with per-game overrides, keyboard and
  controller remapping, hotkeys, fast forward, screenshots, per-scanline
  scaling straight into the window surface, zero-copy dmabuf presentation
  under Wayland with direct scanout when the compositor allows it, and a
  dual-window mode for dual-panel handhelds. No menus yet.

[docs/techniques](docs/techniques) documents the DraStic techniques being
reimplemented, the measurements behind them, and a checklist of which are
done, equivalent, or deliberately not ported.

## Licence

GPLv3 — see [LICENSE](LICENSE). melonDS (GPLv3) is used as reference and, with
attribution, as a code source.

DSperate is a **clean-room** reimplementation of DraStic's *techniques*, documented
by studying its freely-distributable debug-symbol build — which its original
developer, Exophase, approved for distribution — and by measuring it on real
hardware. Those notes are in [docs/techniques](docs/techniques); everything in
them is reconstructible from that binary plus a test device. No DraStic code, in
any form, is in this tree.

## Building

Requires CMake ≥ 3.16, Ninja, and a C++17 compiler; SDL2 for the SDL frontend.

Running it also requires a DS BIOS pair and firmware image (`bios9.bin`,
`bios7.bin`, `firmware.bin`), dumped from your own console. **None are
provided by this repository**, and DSperate has only been tested against an
official dump: no open-source replacement BIOS or firmware has been tried,
and the direct-boot path, KEY1 key table, touchscreen calibration and
firmware settings all read the real ones.

    cmake --preset host && cmake --build --preset host && ctest --preset host
    ./build/host/src/frontend/cli/dsperate --bios9 bios9.bin --bios7 bios7.bin \
        --firmware firmware.bin [--direct game.nds] --frames 60

Cross-building for ARM64 handhelds (needs `aarch64-linux-gnu-g++`; tests run under
`qemu-aarch64-static` if present):

    cmake --preset aarch64-cross && cmake --build --preset aarch64-cross
    (cd build/aarch64 && ctest)

The JIT and NEON kernels only build on AArch64 hosts; everywhere else you get the
interpreter and the portable C++ renderer. On AArch64 the recompiler is the
default for both CPUs (`--interp`, `--jit9`, `--jit7` select otherwise); it is
verified against the interpreter instruction by instruction
(`tests/jit_test.cpp`) and slice by slice on whole games. `DSPERATE_JIT`,
`DSPERATE_NEON`, `DSPERATE_TESTS`, `DSPERATE_CLI` and `DSPERATE_SDL` are the
CMake switches.

The two CPUs are interleaved either in 128-cycle lockstep with melonDS
(`--quantum 128`, the CLI's default — every frame dump and trace comparison
assumes it) or event-bound, each CPU running to the next scheduled event as
DraStic does (`--quantum 0`, the SDL frontend's default; `--lockstep` there
selects the former). Event-bound is a few percent faster at the cost of the
CPUs seeing each other's IPC writes and IRQs up to an event interval late.

## The CLI

`dsperate` is the headless harness: it boots the firmware (or a ROM with
`--direct`), runs `--frames N`, and is what every measurement and comparison
runs through.

    dsperate --bios9 F --bios7 F --firmware F [--direct game.nds] [--frames N]
             [--interp | --jit9 | --jit7] [--quantum N] [--save F] [--replay F]
             [--trace F [--max N]] [--dump-frames F [--dump-from N] [--dump-count N]]
             [--dump-audio F] [--save-state-at N:file] [--load-state F]

`--trace` writes per-CPU instruction traces (`<pc> <instr> <cpsr> r0..r14`,
one line per instruction, spin loops collapsed) for `tools/compare_traces.py`;
`--dump-frames` writes raw framebuffers for `tools/compare_frames.py`, and
`--dump-audio` the raw s16 stereo stream for `tools/compare_audio.py`. The CLI
has no audio output. It does not pick up `<rom>.sav` automatically, so that a
stray `.sav` next to a ROM cannot silently move a frame baseline: give it
`--save file`, which loads read-only and is never written back.

## Playing

`dsperate-sdl` is the SDL2 frontend: direct boot, both screens, sound and
input. It is built when SDL2 is found (`-DDSPERATE_SDL=OFF` to skip it).

    dsperate-sdl game.nds [--bios9 bios9.bin --bios7 bios7.bin --firmware firmware.bin]
                 [--config F] [--scale N] [--fullscreen] [--layout L] [--screen top|bottom]
                 [--dual-window] [--linear] [--lcd-grid S] [--chunky] [--accel] [--no-vsync] [--no-audio]
                 [--volume N] [--no-mic] [--interp] [--lockstep | --quantum N]
                 [--frames N] [--record F | --replay F] [--save F]

### Settings and controls

Settings live in `~/.config/dsperate/dsperate.ini` (`$XDG_CONFIG_HOME` is
honoured; `--config F` names another file), written with every key commented
out on the first run. Two files next to it override any of its keys for one
game: `games/<rom name>.ini` (the ROM's filename without `.nds`, the natural
one to write by hand) and `games/<GAMECODE>.ini` (the four-letter code printed
as `game: ... [XXXX]` at start, shared by every dump of that title); the
filename one wins, and is where a layout picked with the hotkey is
remembered. The command line overrides all of them. `[paths]` holds the BIOS/firmware so they need not
be passed every time, plus optional `saves` and `states` directories (default:
next to the ROM). `[keys]` and `[pad]` remap the DS buttons to SDL key and
controller-button names (`x`, `Right Shift`, `dpup`, `+righttrigger`);
`[hotkeys]` and `[padhotkeys]` bind the frontend's actions, on the controller
usually as `mod+button` (or a chord, `mod+start+back` -- SDL calls Select
"back") with the pad's mode/home button as the modifier. The left stick works
the d-pad; the right stick moves a crosshair over the bottom screen and
clicking it touches (`stylus_axis` = `right|left|none` picks the stick,
`stylus_dpad` names a button that, held, turns the d-pad into the pen;
`stylus_button`, `stylus_speed` pixels per frame at full tilt, `stylus_size`;
it hides after `stylus_hide` idle frames). The touchscreen is driven by a finger or the mouse on the bottom
screen.

Defaults -- keyboard: arrows, `X`/`Z` = A/B, `S`/`A` = X/Y, `Q`/`W` = L/R,
Enter = Start, Right Shift = Select; `Escape` quits, `P` pauses, `Tab` held
fast-forwards, `F` toggles fullscreen, `F4`/`F10` cycle the screen layout forward/back, `F6` swaps
which screen is alone/large/dominant and `F8` moves the PiP inset (all three
remembered for the game), `F9` takes a screenshot (both screens, BMP, in the
states directory), `-`/`=`/`0` are volume down/up/mute, `F5`/`F7` save/load
the state in the current slot and `F2`/`F3` change the slot, `L` closes and
opens the lid, `M` held is the fake microphone. Controller, with Mode held:
Start+Select quits, Start pauses, right trigger fast-forwards, R/L save/load
the state, Right/Left change the slot, Select/X cycle the layout forward/back, Y swaps the screens;
the left stick's click is the microphone.

### Example configs

[`configs/`](configs/) holds three ready-made settings files: `default.ini`
is exactly what the first run writes (regenerated by
`tools/gen_example_configs.sh`, so it cannot drift), and `melonds.ini` /
`drastic.ini` set only the keys that make DSperate feel like those emulators'
defaults -- melonDS's 128-cycle lockstep, interpreter, filtering and
Shift+F1/F1 states; DraStic's toggled fast forward, unfiltered screens and
pad-centred hotkeys; `advdrastic.ini` is Knulli's "Advanced DraStic"
hotkey scheme (Function = the mode button). Copy one over `dsperate.ini` or pass it with `--config`;
`--write-config F` writes the default file anywhere.

### Microphone and lid

`M` (or the pad binding) held feeds noise at 80 % of full scale, for
blowing/shouting prompts on devices without a microphone. Otherwise the real
one is captured -- on Linux straight from ALSA (`mic_dev` / `DS_MIC_DEV`,
default `plughw:0,0`; the handhelds' PipeWire only offers a speaker monitor),
DC-blocked and noise-gated: `mic_gate` is the factor over the tracked floor
(default 5, 0 = off) and `mic_gain` scales what is left (default 0.25, set
against the RG DS and Mario & Luigi's mic-test meter). `--no-mic` leaves it
closed.

Closing the lid puts the game to sleep and opening it wakes it. A handheld
with a real hinge switch (evdev `SW_LID`) drives this itself, and a host
suspend/resume pulses it on hosts without one; `L` toggles it by hand.

### Saves and save states

Battery saves live next to the ROM as `<rom>.sav` (or under `[paths] saves`),
written a second after the game stops writing its save chip and again on
pause, lid close and exit -- a launcher's SIGTERM included.

Save states go to `<GAMECODE>.<slot>.dss` in the states directory (next to
the ROM, or `[paths] states`), ten slots. A state is the whole machine at a
frame boundary (~5.5 MB, uncompressed: RAM, VRAM, both CPUs, every
peripheral, the geometry engine's polygon RAM and the rasterised 3D frame)
and loads only with the same ROM; the battery save is written alongside it so
the two never disagree. The recompiler's translations are dropped on load and
rebuilt as the game runs. `src/core/state/state.h` describes the chunked
format; each subsystem lists its own fields in one `sync_state` that both
writes and reads, so a state saved straight after a load is byte-identical to
the one loaded -- `tools/state_roundtrip.sh <dsperate-cli> <scene> <N> <M>`
checks that, and that the frames after a load match the frames after the
save, on any recorded scene (the CLI takes `--save-state-at N:file` and
`--load-state file`; `DS_STATE_DEBUG=1` prints the cycle-accounting state at
both points). Loading a state is refused during `--record` (the recording
could not replay past it) and `--replay` refuses to load or save states at
all.

Fast forward (`Tab` held, Mode+right trigger, or the `fast_forward_toggle`
hotkey) drops the pacing -- `[emu] ff_speed = N` caps it at N times real time
-- and presents one frame in `ff_skip + 1` (default 3); every frame is still
emulated, so the run stays exact, and the audio queue keeps the newest frames
rather than falling behind.

### Display and handhelds

Layouts (`--layout` / `[video] layout`; `F4`/`F10` step through
`[video] layout_cycle`, by default all of them): `vertical`
(stacked) and `horizontal` (side by side; `--screen` picks which comes
first), `single` (one screen fills the window), `pip` (one fills it, the
other is an inset of `pip_scale` in `pip_corner` = `tl|tr|bl|br`),
`dominant_v` (stacked in DS order, one screen fitted to the width and the
other `dominant_ratio` its size, both centred) and `dominant_h` (side by
side in DS order, fitted to the height, bottoms aligned). Every mode keeps
the 4:3 screen aspect. `--screen top|bottom` (`[video] screen`) is the
screen shown alone, large or dominant; `F6` swaps it.

Under Wayland the core scales each scanline straight into the window surface
as the line is produced (`DS_SCANLINE_SCALE=0/1` overrides the per-driver
default), and when the compositor offers `zwp_linux_dmabuf` the frames are
rendered into CMA dma-heap buffers it composites zero-copy -- or, for a
fullscreen opaque window on an untransformed output, scans out directly on a
hardware plane (`DS_DMABUF=0` disables, `=1` requires). `--dual-window` opens
one fullscreen window per video display with one DS screen each, which is
what a dual-panel handheld wants and what direct scanout needs there.

On a handheld with no desktop session, SDL uses its KMSDRM backend directly;
point `XDG_RUNTIME_DIR` at the PipeWire runtime directory or SDL's PulseAudio
backend spends about twenty seconds failing to connect before sound starts.

## Recording, replay and measuring

`--record scene.dsin` writes what you play, one 16-byte record per frame
(buttons, pen, lid and eight microphone samples -- enough for the games that
measure loudness; older 8-byte logs still replay), and `--replay scene.dsin`
plays it back (in the window, or headlessly with `dsperate --replay
scene.dsin`, which also takes `--dump-frames` and works under `perf`). The
emulator is deterministic given its inputs, so a replay reproduces the
session frame for frame as long as the ROM, BIOS and battery save are the
same as when it was recorded — a played scene becomes a benchmark.
[scenes/](scenes/) holds the recorded scenes used for every measurement in
the docs, with the saves they need; a recording made against a game that
writes a save (Mario & Luigi creates one on boot if it is missing) diverges
immediately without it. The SDL frontend picks up `<rom>.sav` automatically
except under `--replay`, where `--save` names the scene's save and nothing is
written back.

Both frontends print the same per-frame host-time report at the end of a
`--frames` run (`src/core/frame_report.h`): mean, median, p90, p99 and the
count and position of frames over the DS's 16.7 ms budget. Every change is
judged on the tail, not the mean.

## Diagnostics

Environment knobs the core reads; the ones that change timing or output are
for A/B runs, not for play.

- `DS_FPS=1` -- speed, per-stage times and audio buffer depth per second;
  with `--frames N` comparable between runs by frame index.
- `DS_PROFILE=1` (CLI) -- wall time per stage, event counters and the slice
  census at exit; `DS_PROFILE_THREADS=1` per thread.
- `DS_FRAME_HASH=1` (CLI) -- a digest of RAM and both CPUs' registers after
  every frame, and `DS_FRAME_DUMP=<frame>:<path>` writes that frame's RAM, so
  two builds can be diffed to the first frame their *state* differs --
  usually long before the first pixel does.
- `DS_JIT_DENSITY=1` (with `DS_PROFILE=1`) -- host bytes of translated code per
  guest instruction weighted by *execution* rather than by translation, plus the
  distribution of block length by entries. The `[jit] code ... bytes per guest
  instruction` line counts a block translated once and run a million times the
  same as one run once; this does not. Refused alongside `DS_JIT_PRETX`.
- `DS_IO_CENSUS=1` -- the hottest I/O registers by address and CPU, reads and
  writes listed separately, at exit. Reads are the point: a game polling a
  status bit costs a slow-path access per read and leaves no trace in any
  write log.
- `DS_WATCHDOG=<seconds>` (CLI) -- aborts a run whose frame count stops
  advancing for that long, after printing the display-line and raster
  hand-off state.
- `DS_IDLE_SKIP=0|1|all` (or `[emu] idle_skip`) -- the idle-loop skip: `1`
  (default) skips only an ARM9 GXSTAT poll while a swap is pending; `all`
  skips every proven poll loop.
- `DS_JIT_CHURN=1` -- who invalidated translated code and which blocks were
  retranslated, at exit; `DS_JIT_STRICT=1` makes the recompiler test the
  budget before every instruction as the interpreter does, for lockstep
  comparison; `DS_PERF_MAP=1` names translated blocks for `perf`.
- `DS_R3D_THREADS=N` -- raster worker count (0/1 = single-threaded);
  `DS_2D_THREAD=0` keeps engine B on the emulation thread; `DS_2D_LAZY=0`
  renders the 2D engines per line through the same journal. All three must
  produce identical frames -- `tools/scene_hashes.sh` checks that over the
  recorded scenes.
- `DS_QUANTUM`, `DS_LCD_IRQ_DELAY`, `DS_STORE_BUS`, `DS_SPU_BATCH`,
  `DS_R3D_SKIPDUP`, `DS_AUXSPI_LOG`, `DS_MIC_LOG`, `DS_STATE_DEBUG` -- timing
  and logging knobs named where they are read.

## Tests and tools

`ctest` runs the unit tests in [tests/](tests/): the interpreter, the
recompiler against it (under `qemu-aarch64` when cross-built), the NEON
kernels against their portable references, the page table, scheduler, I/O,
SPU, 2D and 3D pipelines and the texture cache. [tools/](tools/) holds the
comparison scripts (`compare_frames.py`, `compare_traces.py`,
`compare_audio.py`), the exactness checks over the recorded scenes
(`scene_hashes.sh`, `state_roundtrip.sh`), `profile_categories.py` for `perf`
output, and `cma_blit_bench` for the dmabuf write-throughput question.
