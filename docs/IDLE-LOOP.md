# Idle-loop skipping

Status: **implemented, verified, and off by default because it does not pay.**
Enable with `DS_IDLE_SKIP=1`. This document records what was measured so the
question does not get re-opened from scratch.

## The idea

`Scheduler::both_idle()` only runs long slices when *both* CPUs are `halted`.
A game that polls a flag instead of using HALT keeps its CPU awake and pays
full price to do nothing. The plan was to widen "idle" to include a CPU
provably spinning, and skip its execution for the slice.

## What the measurement said (gameplay scenes, `--quantum 0`)

Cycle-weighted, with `DS_PROFILE=1`:

| | SM64 | M&L: Bowser's | Meteos |
|---|---|---|---|
| both halted (caught today) | 53.6% | 31.3% | 63.1% |
| one CPU spinning, other halted | 16.7% | 32.3% | 6.8% |
| host time in spin slices | 10.5% | 25.1% | 9.1% |

That looked like a large lever. It is not, because most of that time is not
*skippable*. `DS_SPIN_PCS=1` prints the hot loops with opcodes and the
analyser's verdict; the categories are:

* **Wait-and-copy loops.** The biggest single contributor in Bowser's Inside
  Story polls a flag *and* stores incoming data while advancing two pointers
  (`strcc` + `addcc` + `addcc`). Skipping it would drop the copy. Correctly
  rejected.
* **Loops polling time-sensitive device state** — the cartridge/save ports
  (`AUXSPI`, `ROMCTRL`) and `DISPSTAT`/`VCOUNT`. Allowing these changed frame
  output, so they are excluded. See "the exit-timing problem" below.
* **Thumb loops**, which the analyser does not handle at all.

What remains — provably pure loops polling RAM or an event-driven register —
is worth about 0.1–0.2% of cycles. Measured wall clock is a ~0.6%
*regression* from the per-slice analysis cost, so the default is off.

## What the analyser proves (`src/core/cpu/idle_loop.cpp`)

A loop is skippable only if executing N iterations is indistinguishable from
executing none:

* no stores, no SWI/coprocessor/PSR writes, no calls;
* every load resolves to plain RAM, or to one of a small set of registers that
  change only at a scheduled event (`IPCSYNC`, `IPCFIFOCNT`, `IME`, `IE`,
  `IF`, `GXSTAT`);
* no loop-carried register dependency: the registers the body writes are
  disjoint from those it reads before writing. This is what separates
  `ldr; tst; bne` from `subs r0,#1; bne`, which counts down and must not be
  skipped.

The structural verdict is cached per PC and re-validated by a checksum over the
body, so modified or remapped code falls back to a fresh analysis.

The skip itself only runs when nothing else can change what the loop reads: no
DMA in flight, geometry engine idle, sibling CPU idle, no pending unmasked IRQ.
The slice still ends at the next scheduled event.

## The exit-timing problem (why the big cases are excluded)

Events fire at slice end, then the next slice begins. If an event just
satisfied the loop's exit condition, the loop should leave *early in the next
slice* — but a whole-slice skip delays that exit to the following event. For
RAM-polled loops this never showed up (16/16 games byte-identical); for
`DISPSTAT` it did.

Unlocking those cases needs a **probe-then-skip** slice: run the CPU for a
small budget (~64 cycles) so it can observe the fresh state and exit if it
wants, and only consume the rest of the slice if it is still in the loop.
That is a change to both slice loops including the native JIT state machine,
so it was not attempted here. Expected upside from the one case measured
(`DISPSTAT` in Bowser's) is ~2.6% of cycles in that title and nothing in the
other two.

## Verification

* 16/16 commercial ROMs byte-identical over 600 boot frames, skip on vs off.
* All three gameplay replay scenes byte-identical over 900 frames.
* `ctest` green; aarch64 cross-build clean.

Diagnostics: `DS_PROFILE=1` prints skip counters, per-reason reject counts and
the idle-veto breakdown; `DS_SPIN_PCS=1` prints hot loop PCs with opcodes and
verdicts; `DS_DUMP_CODE=<hex>` dumps 16 guest words once the CPU reaches them;
`DS_IDLE_PORTS=<hex>,...` overrides the allowed register set for bisecting.
