# Differential tracing against melonDS

The interpreter is verified by running the same BIOS/firmware (and later ROM)
on DSperate and on a headless melonDS build, emitting one line per executed
instruction per CPU, and comparing. The 2D renderer is verified the same way
one level up: both emulators dump every frame and the frames are compared
pixel for pixel (see "Frame comparison" below).

## Trace format

One line per instruction, per CPU (`<prefix>.arm9.trace`, `<prefix>.arm7.trace`):

    <pc> <instr> <cpsr> r0 r1 ... r14        (lowercase hex, no 0x)

`pc` is the address of the instruction about to execute; the register state is
the state *before* it executes. Thumb instructions are printed as 16-bit.

**Spin-loop collapse.** A line identical to one of the previous 32 emitted
lines is dropped. A poll loop repeats its exact state every iteration until the
polled value changes, so each iteration after the first vanishes; loops whose
state changes (counters, copies) are kept in full. Both tracers apply the same
rule, so traces shrink by orders of magnitude and stay comparable.

## Producing traces

DSperate:

    dsperate --bios9 bios9.bin --bios7 bios7.bin --firmware firmware.bin \
             --frames 120 --trace out/ds --max 4000000 [rom.nds]

melonDS: a headless tracer lives outside this repo (it links melonDS's core,
with a ten-line trace hook patched into `ARM.cpp`). Its output uses the same
format and the same collapse rule.

## Comparing

    tools/compare_traces.py ref.arm7.trace ds.arm7.trace --window 20000

The comparator streams both files and reports only *semantic* divergences.
When lines differ it searches up to `--window` lines ahead on each side for an
identical line followed by `--confirm` (default 8) matching lines, and resumes
there, counting a "timing resync". Timing-only differences (an I/O poll seeing
the other CPU's write one iteration earlier or later, an interrupt landing a
few instructions apart) resync; a genuine CPU or I/O bug does not, and is
printed with context and a field-by-field diff.

## Direct boot

`--direct` (both tracers) skips the firmware: the ROM's binaries are loaded,
the header and firmware user settings are placed in main RAM, CP15/PU state is
set up and both CPUs start at their entry points in SVC mode, exactly as
melonDS does it. Use it for games; the firmware menu needs touch input.

## Per-frame instruction counts

`TRACE_PER_FRAME=1` makes both tracers print `frame N arm9 X arm7 Y` per
frame. This is the quickest way to see *relative speed* problems, which the
trace diff only shows indirectly (as poll loops exiting at different
iterations). It found the half-speed clock bug in minutes after the diff had
pointed at the wrong layer for hours.

## Frame comparison

Both tracers take `--dump-frames file`: after every frame they append the
top and bottom screens as 256x192 little-endian `0xAARRGGBB` words (the
same 6-to-8-bit expansion melonDS's software renderer produces, so a correct
frame is byte-identical).

    dsperate --bios9 .. --bios7 .. --firmware .. --direct --frames 300 \
             --dump-frames out/ds.frames game.nds
    tools/compare_frames.py out/ref.frames out/ds.frames [--offset K] [--png dir]

## Audio comparison

`--dump-audio file` appends the SPU output after every frame: raw interleaved
s16 stereo at 32768 Hz, ≈547 samples per frame (import into Audacity as
"raw, signed 16-bit PCM, little-endian, 2 channels, 32768 Hz", or play with
`aplay -f S16_LE -c 2 -r 32768`). The melonDS tracer build takes the same
flag and writes the mixer output before its resampler, so the streams are
comparable sample for sample:

    tools/compare_audio.py out/melon.pcm out/ds.pcm [--offset K] [--search N]

Without `--offset` the candidate is aligned by searching for the shift with
the fewest mismatches over the first non-silent seconds (streams start a few
ms apart between emulators); it then prints mismatching samples per frame,
the first differing samples, and the RMS of the difference against the
signal. `DS_DEBUG_SPU=1` logs SOUNDCNT/capture writes and every channel
key-on (with the sample index it lands on) to stderr, which is how a
mismatch is traced back to a register write.

The comparator prints per-frame mismatch counts and the first differing
pixel, and with `--png` writes ref/cand/diff images for the first mismatching
frames. Games drift by a few frames against melonDS (ARM7-driven waits differ
slightly per scene), so `--offset K` compares candidate frame N with reference
frame N+K; try a few offsets before suspecting the renderer. Differences that
survive every offset and sit in fades, typewriter text or sprite animation are
phase, not rendering.

Debug hooks in the CLI, all environment-gated and free when unset:

| variable | effect |
|---|---|
| `DS_DEBUG_GPU=1` | one line per frame: POWCNT, DISPCNT A/B, master brightness, DISPCAPCNT, VRAMCNT |
| `DS_DEBUG_DUMP_FRAME=N` (`DS_DEBUG_DUMP_LINE=L`) | dump both engines' registers/latches and render line L at frame N |
| `DS_DEBUG_VRAMNZ=1`, `DS_DEBUG_VRAMCNT=1`, `DS_DEBUG_GPUREG=1` | per-frame bank fill, VRAMCNT writes, blend/brightness register writes |
| `DS_DEBUG_GX=1` | one line per VBlank: 3D power/flush state, polygon and vertex counts, DISP3DCNT, clear attributes, FIFO level, GXSTAT, ARM9 IE/IF |
| `DS_PROFILE=1` | wall time per stage at exit (CPUs, DMA, geometry, each 2D line stage, output, 3D clear/spans/final pass); `perf` is not usable in every sandbox, this always is |
| `DS_WATCH=<hex>` | log writes to a main-RAM or VRAM word with PC, frame and line |
| `TRACE_PC_HIST=1` | uncollapsed PC histogram per CPU at exit (what a "quiet" frame is doing) |
| `DS_DEBUG_SLICES=1` | one line per scheduler slice: time, ARM9 cycles run, both pcs and budgets — diff two engines' outputs to find the first slice they disagree on |
| `DS_DEBUG_CYCLES=1` | the budget before every instruction (interpreter, and the JIT in strict mode): the instruction-level version of the slice diff |
| `DS_JIT_STRICT=1` | recompiler tests the budget after every instruction, so it interleaves exactly like the interpreter (verification mode) |
| `DS_JIT_DEBUG=1` | log every recompiler fallback to the interpreter with the resulting state; dump translated blocks |
| `DS_JIT_HIST=1` | with `DS_PROFILE=1`: recompiler counters and hottest fallback sites at exit |
| `DS_JIT_FASTCOST=1`, `DS_QUANTUM=<n>` | measurement knobs (inexact timing): constant data cost / scheduler quantum |
| `TRACE_START_FRAME=N` | start tracing at frame N (both tracers) |

The melonDS tracer adds `TRACE_VRAM_STATS` and `TRACE_VRAM_PER_FRAME` (bank
fill and VRAMCNT), which is how the Kirby DMA ordering bug below was pinned.

## Dead-value masking

When pc/instr/cpsr match but a register differs (a poll-loop counter after the
loop exits at a different iteration), the comparator masks that register and
keeps going; it unmasks when the values agree again. `--mask-regs` sets the
budget (default 2, 0 disables).

## What it has found so far (2026-08-20)

Firmware boot (120 frames) and Meteos direct boot (300 frames), 4 M distinct
states per CPU: no semantic divergence. Bugs it caught on the way, in order:

1. Branch to `pc+4` in Thumb indistinguishable from "no branch" in the run
   loop (fixed with an explicit `jumped` flag).
2. Thumb IRQ/FIQ link register off by two.
3. Empty-slot cart data must read as zero, and ROMCTRL's DRQ/busy bits must
   follow the transfer timing, or the BIOS header copy diverges.
4. `Scheduler::now()` must include the running CPU's consumed cycles, or
   events scheduled from inside an instruction land a whole slice early.
5. Games probe Wi-Fi RAM and the baseband/RF registers at boot; without them
   the ARM7 takes a different init path.
6. HALTCNT (0x04000301) must halt the ARM7, or it spins instead of sleeping.
7. Scanline/timer constants are system-clock cycles; the scheduler counts ARM9
   cycles. Everything ran at half speed relative to the CPUs.
8. The ARM7's slice overshoot must carry into the next slice, or it gains up
   to one memory access per 64 cycles (~10% speed).
9. melonDS specifics worth matching for lockstep: IRQ entry sets CPSR to 0xD2
   (F bit too); RTC resets to 2000-01-01 with status1 = 0x82; an empty or
   unpowered cart slot reads as zero.

Found by the frame comparison (2026-08-21). Every one of these was invisible
to the instruction diff, because a game that is stuck or idling collapses to
the same few lines on both sides:

10. VRAMCNT_H/I live at 0x248/0x249 with WRAMCNT at 0x247 in between; indexing
    them as banks 8 and 9 overflowed into POWCNT1 (screens went dark) and
    mapped banks H/I from the wrong registers.
11. `SWPB` decoded as undefined: the decoder required bits 22:21 clear, but
    bit 22 *is* the byte flag. Mega Man ZX parked in the undefined-instruction
    handler.
12. The hardware divider and square-root unit (0x04000280-0x040002BF) were
    missing; Meteos computes its fade step with a division and got zero.
13. The geometry-FIFO interrupt: games arm GXSTAT bits 30-31 and run their
    frame logic from IRQ 21. Until the 3D engine exists the FIFO is modelled
    as always empty and the IRQ raised accordingly.
14. An immediate DMA must stall the CPU that started it *now*, not at the next
    scheduler slice. Kirby maps a bank to LCDC, starts the DMA and restores
    the mapping ten instructions later; with the DMA deferred, the writes hit
    a bank that was no longer LCDC-mapped and were dropped
    (`Scheduler::preempt`).
15. POWCNT's screen-enable bit is latched at frame start; the first frame
    after reset/direct boot has no line-0 event, so it needs an explicit
    latch.

Status after these: Meteos (300 frames), Mega Man ZX, Bangai-O Spirits and
Sonic Rush (400 frames each) are pixel-exact against melonDS apart from one or
two scene-transition frames at a constant offset; Kirby Canvas Curse matches
except for fade/typewriter phase; Rhythm Heaven differs only where it draws
with the 3D engine. The AArch64 build under qemu produces byte-identical
frames to the host build.

With the 3D engine (2026-08-21): Rhythm Heaven's 3D title logo is
pixel-exact at a constant 2-frame offset (3 transition frames of 400 differ);
Super Mario 64 DS's rotating star (textured, lit, anti-aliased) is exact at
offset 0 then offset 1 after a mid-run skew change; Geometry Wars differs on
4 transition frames. Star Fox Command's Nintendo-logo quad (rendered through
display capture with per-frame screen swapping) is exact in content but runs
6 frames ahead of melonDS; the trace shows melonDS's ARM9 spending those
frames in a file-name search loop that DSperate finished earlier, so the skew
is cartridge-load timing, not rendering (see Limits). Over 1200 frames:
Mario & Luigi: Bowser's Inside Story 0 frames differ at offset 1, Metroid
Prime Hunters 0 at offset 0, Spectrobes 6 at offset 2; Golden Sun: Dark
Dawn's 3D title scene (textured terrain, drifting translucent clouds, both
screens through display capture) is exact on the top screen at offset -6 and
on the bottom screen apart from its pulsing "tap the screen" text, whose
phase is not tied to the frame counter; Kingdom Hearts 358/2 Days, Okamiden
and Metroid Prime Pinball differ only in fade steps and moving sprites
(phase). The AArch64 build remains byte-identical to the host on these.

## Recompiler vs interpreter (2026-08-21)

The AArch64 build takes `--interp` (interpreter for both CPUs), `--jit9` and
`--jit7` (one CPU recompiled); the default recompiles both. The recompiler
calls the same trace hook per instruction when tracing is on, so every tool
above applies to it unchanged. The sharpest check is the slice diff:

    DS_DEBUG_SLICES=1 dsperate ... --interp  2> a.txt
    DS_DEBUG_SLICES=1 DS_JIT_STRICT=1 dsperate ... 2> b.txt
    cmp a.txt b.txt          # first differing line = first slice that differs

followed by `DS_DEBUG_CYCLES=1` on both to see the instruction. This is how
the interpreter's own Thumb fetch-cost bug was found: strict-mode frames were
identical but the cycle totals were 182 apart over 30 frames, the slice diff
pointed at the BIOS CpuSet loop, and the cycle log showed `lsrs` costing 0 on
one side and 1 on the other. The per-instruction fuzzer (`test_jit`, cross-built,
`ctest` runs it under qemu) covers the inlined instruction forms directly and
prints the shortest failing sequence.

## Toolchain hazards

- **GCC 13.3 AArch64, `-O2`: a side-effecting member function deleted.** With
  the colour-effects pass written as a member function looping over
  `std::array` members of `*this`, `ipa-modref` summarised its stores as
  parameter writes, `ivopts` rewrote the addressing, and `dce2` removed the
  call from `render_line` altogether — the composite stage silently never ran
  on AArch64 while the x86-64 build of the same source was fine.
  `-fno-ipa-modref` or `-fno-ivopts` restores the call; so does writing the
  pass as a free function over plane pointers, which is what the tree does now
  (it is the shape the NEON kernels want anyway). Two things guard against a
  recurrence: `test_gpu` fails under qemu when it happens, and the AArch64
  build's frame dumps are compared byte-for-byte with the host's.

## Limits

- Firmware boot: the instruction streams match but melonDS reaches the
  POWCNT write that lights the screens ~80 frames later than DSperate (an
  ARM7-side wait the firmware performs is faster here). Not yet investigated;
  it does not affect direct-booted games.
- Cartridge-heavy scenes run ahead of melonDS: Star Fox Command reaches its
  Nintendo logo 6 frames early (melonDS's ARM9 spends ~43 k iterations of a
  string-compare loop at 0x02086e40 during frames 146-152 that DSperate has
  already finished). The per-transfer cart timing has not been compared in
  isolation yet; this is the next timing item to pin down.

- Timing follows melonDS's model (see ARCHITECTURE.md §4). On Meteos over
  300 frames, 84 frames have the exact ARM9 instruction count and 50 the
  exact ARM7 count; totals agree to 0.7% on both CPUs. The residue shows up
  as timing resyncs, not divergences; the known modelling difference is the
  instruction-cache approximation.
- Resync windows are a heuristic. A value that differs but is dead (never read
  again) cannot resync until it is overwritten; the comparator reports it as a
  divergence and the reader has to judge.
