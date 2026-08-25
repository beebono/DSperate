# Plan: the CPU emulation, which is the whole gap

## The number

Same ROM, same starting state, 300 frames from direct boot, RK3566, DraStic's
own `--benchmark 300` ablation against our `DS_PROFILE=1` stage timers (ours
scaled by 8.28/10.02 to remove profiling overhead). See
[profile-vs-drastic.md](profile-vs-drastic.md).

| | ours | DraStic | ratio |
|---|---|---|---|
| **CPU (both cores, incl. everything the guest triggers)** | **4.11 ms** | **0.80 ms** | **5.2×** |
| 2D engines | 1.92 | 1.99 | 1.0× |
| 3D raster (critical path) | 0.88 | 1.07 | 0.8× |
| 3D geometry | 0.14 | 0.13 | 1.1× |
| SPU | 0.37 | 0.03 | 11.4× |
| **whole frame** | **8.28** | **2.98** | **2.8×** |

Per guest instruction, at ~350 k guest instructions a frame across both cores:
**23.4 host cycles for us, 4.6 for DraStic.**

CPU is **50 % of our frame and 27 % of theirs**. Close the CPU gap alone and
the frame goes from 8.28 ms to about 5.0 ms; nothing else on the list moves it
by more than a few percent.

## Why this was missed for so long

[jit-technique-audit.md](jit-technique-audit.md) ends with *"nothing left in
the recompiler is worth more than about 2 %, and the 3D rasteriser is ~35 % of
the frame"*. Both halves of that sentence are `perf` self-time shares, and both
are misleading for the same two reasons:

1. **`perf` samples every thread.** The 3D raster runs on three workers plus a
   share on the emulation thread, so its *CPU time* is large while its
   contribution to *wall time* is one band's worth. Ranking by sample share
   inflates it against work that is strictly serial.
2. **CPU cost does not aggregate under any symbol.** Translated guest code is
   its own `[JIT]` DSO and looks like ~15 %. Everything the guest *triggers* —
   `Io::read`, `PageTable::remap`, `Timing::update_cpu9`, `Scheduler::slice_next`,
   the memory helpers — is scattered across dozens of symbols in our own DSO
   and never adds up to anything that reads as "CPU". Only a timer bracketing
   the whole CPU run shows it as one number.

The audit's own §A–§F already identified the real divergences and priced two of
them at "larger than DraStic's entire CPU-emulation budget". They were then
ranked below renderer work by a frame-share number that could not see them.

**Rule going forward: bracket a subsystem with a timer before ranking it.
Per-symbol shares cannot price a cost spread across fifty symbols, and cannot
compare a threaded subsystem against a serial one.**

## Where the 23.4 cycles go

The audit measured the emitted code directly: our hot sections emit **36.8
bytes — about nine host instructions — per guest instruction**, against a
one-instruction guest ALU op. That is the symptom; §A–§F are the causes, in
the audit's own words and its own ordering, now re-priced against a 4.11 ms
budget rather than a 2 % frame share.

### A. Runtime cycle accounting on every memory access — *unpriced, largest*

The audit calls this "the single biggest divergence": we spend **16
instructions where the technique spends 6**, on every memory access, because
the exact melonDS cycle model is evaluated inline. Seven of the extra ten are
the ARM7/ARM9 split.

It is unpriced because `DS_JIT_FASTCOST=1` is not a timing instrument (it runs
50× slower — it is the fuzzer's mutation check) and any cheaper model changes
frame output, which makes an A/B suspect.

**How to price it honestly:** build a variant that keeps the model but hoists
the per-access work — a per-block precomputed cost where the block's accesses
are statically known, falling back to the full model otherwise — and diff
frames. If frames stay identical the comparison is clean. If they cannot be
kept identical, price it as an upper bound with a deliberately wrong model and
treat the number as a ceiling, not a result.

### B. The indirect-branch dispatcher — 47 instructions against nine

Measured at 1.39 % (sm64) / 2.78 % (meteos) of the process, which at the time
looked small. Against the CPU budget it is ~3–6 % of CPU. The audit proved the
cost is the `br` mispredict on a *shared* stub, not instruction count —
removing eight instructions bought nothing. **So the fix is fewer dispatches or
a per-site probe, not a shorter one.**

### C. Every CPU switch leaves translated code

The audit: `slice_next` at ~300 cycles per slice, and the total *"larger than
DraStic's entire CPU-emulation budget"*. Under `--quantum 0` entries are
~620/frame; under the 128-cycle lockstep default, ~10,400. **Refilling the
budget in the poll stub so a switch does not leave the arena** is item (7) on
the audit's list and is worth re-pricing now that the denominator is CPU time
rather than frame time.

### D. Conditional execution always branches

Every conditional guest instruction becomes a branch; a `csel` is one
instruction and never mispredicts. ARM code is dense with conditional ALU ops,
so this is a broad, mechanical win across the whole translation.

### E. Flag merging through a `bl` to an `mrs`/`msr` stub

Two ~6-cycle system-register moves plus a call, where the flags could stay in
NZCV.

### F. LDM/STM

Straddle probability against a 2 KB page is ~6 %, and stacks straddle more
often. Currently the whole instruction takes the slow path.

## Order of work

Ranked by expected value against the 4.11 ms, not by novelty. Every step gets a
byte-identical frame check and a **paired** benchmark (see the rig notes: pair
the interleaved reps, ~0.09 % resolution at 12 reps).

1. **Price §A.** It is the largest single divergence and the only one still
   unmeasured. Until it has a number the rest of the ordering is guesswork.
   Even a ceiling is progress.
2. **§D, conditional execution → `csel`.** Mechanical, broad, low risk, and
   independently verifiable with the existing strict slice diff.
3. **§C, keep the budget refill inside the arena.** Re-price first — the
   audit's own note says it is worth less than it looks under `--quantum 0`,
   but the SDL frontend defaults to that and the CLI does not.
4. **§B, per-site indirect-branch probes.** Higher risk, and the audit already
   showed the obvious version of this change does nothing.
5. **§E and §F.** Small and local; do them when touching that code anyway.

The ITCM dispatch tables (audit §1) remain the largest *new* mechanism, but
they are a bigger change than any of the above and should wait until §A is
priced, in case the memory path turns out to dominate.

## What success looks like

- CPU stage time per frame, `DS_PROFILE=1`, direct-boot SM64DS 300 frames,
  against the 4.11 ms baseline here.
- Cycles per guest instruction, against 23.4 now and 4.6 for DraStic.
- Whole-frame ms against 8.28, compared to DraStic's 2.98 on the identical
  workload — the only comparison that is not scale-free.

Halving the CPU gap takes the frame to ~6.2 ms. Reaching DraStic's CPU cost
takes it to ~5.0 ms, at which point the renderer becomes the majority again and
the parked [span-batching branch](../README.md) is worth revisiting.
