# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Claude Code is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

An attempt at a Nintendo DS emulator that reimplements DraStic JIT + NEON Rendering
techniques combined with modern niceties that melonDS provides like per-screen windows
and maybe overlaid shader support later on.

---

**Status: 2D and 3D video, no sound.** Both CPUs (ARM946E-S / ARM7TDMI) are
interpreted with melonDS-grade cycle timing; DMA, timers, IPC, SPI devices,
RTC, Wi-Fi probing, the divider/sqrt unit and a retail cartridge (KEY1, save
chip, direct boot) are in. The two 2D engines are complete in portable C++
(all BG modes, sprites, windows, mosaic, blending, extended palettes, display
capture, master brightness), and the 3D engine — command FIFO with cycle
timing, matrix stacks, lighting, clipping, and a software rasteriser with
textures, shadows, fog, edge marking and anti-aliasing — renders commercial
games' 3D scenes pixel-for-pixel like melonDS
([docs/TRACING.md](docs/TRACING.md)); the NEON kernels and the SPU are next.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes the design.

## Licence

GPLv3 — see [LICENSE](LICENSE). melonDS (GPLv3) is used as reference and, with
attribution, as a code source.

DSperate is a **clean-room** reimplementation of DraStic's *techniques*, documented
by studying the binary, in private research notes that are not published. No DraStic code, in any
form, is in this tree.

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
(`tests/jit_test.cpp`) and slice by slice on whole games (docs/TRACING.md).

Sound is mixed by the core at 32768 Hz (`src/core/spu/`); the CLI has no audio
output but `--dump-audio file` writes the raw s16 stereo stream.

## Playing

`dsperate-sdl` is the SDL2 frontend: direct boot, both screens stacked, sound
and input. It is built when SDL2 is found (`-DDSPERATE_SDL=OFF` to skip it).

    dsperate-sdl game.nds --bios9 bios9.bin --bios7 bios7.bin --firmware firmware.bin \
                 [--scale N] [--fullscreen] [--linear] [--no-vsync] [--no-audio]
                 [--interp] [--frames N]

Keyboard: arrows, `X`/`Z` = A/B, `S`/`A` = X/Y, `Q`/`W` = L/R, Enter = Start,
Right Shift = Select, `F` toggles fullscreen, Escape quits. A game controller
is picked up automatically (Select+Start quits, for handhelds without a
keyboard), and the touchscreen is driven by a finger or the mouse on the
bottom screen. Battery saves live next to the ROM as `<rom>.sav`; there are no
savestates. `DS_FPS=1` prints speed, per-stage times and audio buffer depth;
with `--frames N` the output is comparable between runs by frame index.

`--record scene.dsin` writes what you play, one 8-byte record per frame, and
`--replay scene.dsin` plays it back (in the window, or headlessly with
`dsperate --replay scene.dsin`, which also takes `--dump-frames` and works
under `perf`). The emulator is deterministic given its inputs, so a replay
reproduces the session frame for frame as long as the ROM, BIOS and battery
save (`<rom>.sav`) are the same as when it was recorded — a played scene
becomes a benchmark.

On a handheld with no desktop session, SDL uses its KMSDRM backend directly;
point `XDG_RUNTIME_DIR` at the PipeWire runtime directory or SDL's PulseAudio
backend spends about twenty seconds failing to connect before sound starts.
