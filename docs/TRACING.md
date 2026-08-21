# Differential tracing against melonDS

The interpreter is verified by running the same BIOS/firmware (and later ROM)
on DSperate and on a headless melonDS build, emitting one line per executed
instruction per CPU, and comparing.

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

## Limits

- Timing follows melonDS's model (see ARCHITECTURE.md §4) and agrees to
  ~0.1% (ARM9) / ~3% (ARM7) in instructions per frame on Meteos. The ARM7
  residue shows up as timing resyncs, not divergences.
- Resync windows are a heuristic. A value that differs but is dead (never read
  again) cannot resync until it is overwritten; the comparator reports it as a
  divergence and the reader has to judge.
