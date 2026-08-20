# ARM → AArch64 recompiler

Not started. This directory only builds on AArch64 hosts (`DSPERATE_JIT`).

The design it will implement is summarised in `docs/ARCHITECTURE.md` §3 (the
full derivation is in private research notes that are not published).

**Every line here is authored from that design description.** Nothing is
transcribed from `objdump`/Ghidra output of any proprietary binary. melonDS's
`ARMJIT_A64` (GPLv3) may be consulted and, with attribution, reused.

Planned pieces:

- `runtime.S` — entry/exit, CPU switch, memory slow-path trampolines, indirect
  branch, alerts. Hand-written AArch64.
- `emit.h/.cpp` — AArch64 instruction encoder (or reuse melonDS's `Arm64Emitter`).
- `translate.cpp` — one-pass translator: pinned guest registers, guest flags in
  host NZCV, inverted `B.cc` for conditionals, constant tracker, batched cycle
  `SUB`, block prologue with dual entry points, direct links as bare `B`.
- `cache.cpp` — translation cache arenas, 32-bit block offsets, block
  descriptors, lookup tiers, PC-metadata tables for PC reconstruction.
