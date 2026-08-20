# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Claude Code is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

An attempt at a Nintendo DS emulator that reimplements DraStic JIT + NEON Rendering
techniques combined with modern niceties that melonDS provides like per-screen windows
and maybe overlaid shader support later on.

---

**Status: scaffolding.** The tree builds and its tests pass on x86-64 and AArch64,
but nothing is emulated yet — the interpreter is a stub. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design the code is growing into.

## Licence

GPLv3 — see [LICENSE](LICENSE). melonDS (GPLv3) is used as reference and, with
attribution, as a code source.

DSperate is a **clean-room** reimplementation of DraStic's *techniques*, documented
by studying the binary, in private research notes that are not published. No DraStic code, in any
form, is in this tree.

## Building

Requires CMake ≥ 3.16, Ninja, and a C++17 compiler.

    cmake --preset host && cmake --build --preset host && ctest --preset host
    ./build/host/src/frontend/cli/dsperate --frames 60 [rom.nds]

Cross-building for ARM64 handhelds (needs `aarch64-linux-gnu-g++`; tests run under
`qemu-aarch64-static` if present):

    cmake --preset aarch64-cross && cmake --build --preset aarch64-cross
    (cd build/aarch64 && ctest)

The JIT and NEON kernels only build on AArch64 hosts; everywhere else you get the
interpreter and the portable C++ renderer.
