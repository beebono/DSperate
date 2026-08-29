# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Agentic coding is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

An attempt at a Nintendo DS emulator that reimplements DraStic JIT + NEON Rendering
techniques combined with modern niceties that melonDS provides like per-screen windows
and maybe overlaid shader support later on.

---

**Status: 2D and 3D video, sound, and an AArch64 recompiler.** Both CPUs
(ARM946E-S / ARM7TDMI) run under the recompiler on AArch64 hosts and under the
interpreter everywhere else, with melonDS-grade cycle timing; DMA, timers, IPC,
SPI devices, RTC, Wi-Fi probing, the divider/sqrt unit and a retail cartridge
(KEY1, save chip, direct boot) are in. The two 2D engines are complete (all BG
modes, sprites, windows, mosaic, blending, extended palettes, display capture,
master brightness), and the 3D engine — command FIFO with cycle timing, matrix
stacks, lighting, clipping, and a software rasteriser with textures, shadows,
fog, edge marking and anti-aliasing — renders commercial games' 3D scenes
pixel-for-pixel like melonDS. The SPU mixes at 32768 Hz and the SDL frontend
plays it.

[docs/techniques](docs/techniques) documents the DraStic techniques being
reimplemented and the measurements behind them;
[src/core/cpu/jit/README.md](src/core/cpu/jit/README.md) describes the
recompiler as built.

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

Requires CMake ≥ 3.16, Ninja, and a C++17 compiler.

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
(`tests/jit_test.cpp`) and slice by slice on whole games.

The two CPUs are interleaved either in 128-cycle lockstep with melonDS
(`--quantum 128`, the CLI's default — every frame dump and trace comparison
assumes it) or event-bound, each CPU running to the next scheduled event as
DraStic does (`--quantum 0`, the SDL frontend's default; `--lockstep` there
selects the former). Event-bound is a few percent faster at the cost of the
CPUs seeing each other's IPC writes and IRQs up to an event interval late.

Sound is mixed by the core at 32768 Hz (`src/core/spu/`); the CLI has no audio
output but `--dump-audio file` writes the raw s16 stereo stream.

## Playing

`dsperate-sdl` is the SDL2 frontend: direct boot, both screens stacked, sound
and input. It is built when SDL2 is found (`-DDSPERATE_SDL=OFF` to skip it).

    dsperate-sdl game.nds --bios9 bios9.bin --bios7 bios7.bin --firmware firmware.bin \
                 [--scale N] [--fullscreen] [--linear] [--no-vsync] [--no-audio] [--no-mic]
                 [--interp] [--lockstep | --quantum N] [--layout vertical|horizontal]
                 [--frames N] [--record F | --replay F]

Keyboard: arrows, `X`/`Z` = A/B, `S`/`A` = X/Y, `Q`/`W` = L/R, Enter = Start,
Right Shift = Select, `F` toggles fullscreen, `L` closes and opens the lid
(the game sleeps and wakes; a handheld with a real hinge switch drives this
itself), `M` (or a controller's right trigger) held is a fake microphone (noise at 80 % of full scale, for
blowing/shouting prompts on devices without one; the real one is captured
otherwise -- on Linux straight from ALSA, `DS_MIC_DEV`, default `plughw:0,0`, DC-blocked and noise-gated -- `DS_MIC_GATE` factor over the tracked floor, default 5, 0 = off -- `DS_MIC_GAIN` to scale, default 0.25 (set against the RG DS and Mario & Luigi's mic-test meter); since
the handhelds' PipeWire only offers a speaker monitor; `--no-mic` to leave it closed), Escape quits. A game controller
is picked up automatically (Select+Start quits, for handhelds without a
keyboard), and the touchscreen is driven by a finger or the mouse on the
bottom screen. Battery saves live next to the ROM as `<rom>.sav`; there are no
savestates. `DS_FPS=1` prints speed, per-stage times and audio buffer depth;
with `--frames N` the output is comparable between runs by frame index.
`DS_FRAME_HASH=1` (CLI) prints a digest of RAM and both CPUs' registers after
every frame, and `DS_FRAME_DUMP=<frame>:<path>` writes that frame's RAM, so
two builds can be diffed to the first frame their *state* differs -- usually
long before the first pixel does. `DS_IDLE_SKIP=0|1|all` sets the idle-loop
skip: `1` (default) skips only an ARM9 GXSTAT poll while a swap is pending;
`all` skips every proven poll loop. `DS_JIT_CHURN=1` prints, at exit, who invalidated
translated code and which blocks were retranslated. `DS_WATCHDOG=<seconds>` (CLI)
aborts a run whose frame count stops advancing for that long, after printing the
display-line and raster hand-off state -- the log then holds what a debugger on
the stuck process would have shown.

`--record scene.dsin` writes what you play, one 16-byte record per frame
(buttons, pen, lid and eight microphone samples -- enough for the games that
measure loudness; older 8-byte logs still replay), and
`--replay scene.dsin` plays it back (in the window, or headlessly with
`dsperate --replay scene.dsin`, which also takes `--dump-frames` and works
under `perf`). The emulator is deterministic given its inputs, so a replay
reproduces the session frame for frame as long as the ROM, BIOS and battery
save are the same as when it was recorded — a played scene becomes a benchmark.

The SDL frontend picks up `<rom>.sav` automatically; the CLI does not, so that
a stray `.sav` next to a ROM cannot silently move a frame baseline. Give it
`--save file` instead, which loads read-only and is never written back — a
replay must not mutate its own input. A recording made against a game that
writes a save (Mario & Luigi creates one on boot if it is missing) will diverge
immediately without it.

On a handheld with no desktop session, SDL uses its KMSDRM backend directly;
point `XDG_RUNTIME_DIR` at the PipeWire runtime directory or SDL's PulseAudio
backend spends about twenty seconds failing to connect before sound starts.
