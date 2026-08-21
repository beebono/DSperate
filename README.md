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
