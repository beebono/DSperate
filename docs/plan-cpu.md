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

## The split inside that 4.11 ms, measured 2026-08-25

The rule at the end of the next section -- bracket a subsystem before ranking
it -- was applied once, at the CPU boundary, and then not applied *inside* it.
Six suspects were ranked with no numbers against the budget. Here are the
numbers: `perf` restricted to the **emulation thread** (the three render
workers are 42 % of all samples on sm64 and are what inflated the renderer in
the old ranking), bucketed by defining object with
[../tools/profile_categories.py](../tools/profile_categories.py).

| bucket | sm64 % of CPU | meteos % of CPU |
|---|---|---|
| **translated guest code** | **50.6** | **56.3** |
| **jit stubs** | **16.1** | **16.4** |
| **IO registers** | 14.8 | 10.0 |
| memory (bus/pagetable/timing) | 6.1 | 4.0 |
| jit helpers | 4.5 | 3.0 |
| jit runtime C++ | 3.9 | 6.9 |
| interpreter fallback | 3.9 | 3.3 |

Half the CPU budget is the code we emit, which is where A, D and E live.

**Splitting translated code by CPU is the surprise.** The ARM7 takes **34.3 %**
(sm64) and **33.2 %** (meteos) of translated-code time while executing **15 %**
of the guest instructions -- **~2.9x costlier per instruction than the ARM9**,
on both scenes, and not something any section below separates out.

### Denominators, from a host census

`DS_CENSUS=1` under `--interp` counts the executed stream by shape. Counting is
a workload property, so it needs no device. sm64, per frame:

| | |
|---|---|
| guest instructions | **212,538** (ARM9 180,391 + ARM7 32,147) |
| cost-model runs (A's denominator) | **83,530** |
| ...in the same 4 KB page as the previous run | **65 %** |
| ...pc-relative, page known at translate time | **5.7 %** |
| csel-able conditional dp (D's denominator) | **10,700 = 5.0 % of executed** |

Note these are the **1800-frame replays**, not the 300-frame direct boot the
table above uses. Direct-boot sm64 runs ~449 k guest instructions a frame
against the replay's ~212 k; the two are different programs and the numbers do
not interchange. The rig benchmarks the replays.

### What this does to the ordering

- **A is confirmed largest and is now priced** -- see below.
- **C is not a target.** The scheduler is 3.2 % of the emulation thread and
  sits outside the CPU buckets entirely; under `--quantum 0`, which both
  frontends and `bench3.sh` use, entries are 661/frame. It is already in its
  good configuration.
- **D is worth ~0.4 % of frame**, not second place. 10,700 csel-able
  instructions a frame is ~1 % of CPU even if every one saved a full 10-cycle
  mispredict. It was ranked on "mechanical and broad" without a denominator.
- **The A fix changes shape.** The plan proposed static resolution where the
  page is known at translate time; that is 5.7 % of accesses. The locality is
  the lever, and on the ARM7 the answer turned out to be better still.

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

### A. Runtime cycle accounting on every memory access — *priced 2026-08-25*

The audit calls this "the single biggest divergence": we spend **16
instructions where the technique spends 6**, on every memory access, because
the exact melonDS cycle model is evaluated inline. Seven of the extra ten are
the ARM7/ARM9 split.

**It is priced, and the plan was wrong that it could not be.** The instrument
is `DS_JIT_COSTPROBE`, which emits the cost sequence **twice**, the first
copy's result discarded into a dead scratch. The budget is still charged
exactly once, so frames are byte-identical and the emulated cycle count is
identical to the digit (336114004 either way at 300 frames) — the two arms run
the same workload, which is what made every earlier attempt suspect. The
fuzzer passes in every probe mode, and it checks cycle counts against the
interpreter.

sm64, 1800 frames, paired interleaved reps:

| probe | delta frame | SE | t | slower in |
|---|---|---|---|---|
| **whole model, both CPUs** | **+5.58 %** | 0.13 % | 42 | 12/12 |
| ARM9 only | +2.62 % | 0.18 % | — | 6/6 |
| ARM7 only | +2.56 % | 0.12 % | — | 6/6 |
| timing-table lookup only | +3.08 % | 0.17 % | 18 | 6/6 |
| combine arithmetic only | +2.49 % | 0.04 % | 59 | 5/5 |

Both splits are additive to within noise (2.62 + 2.56 = 5.18; 3.08 + 2.49 =
5.57 against 5.58), which is the check that the attribution means anything.

**Read every one of these as a ceiling.** The shipped copy is deliberately
scheduled into the shadow of the page-table and data loads; the probe's
duplicate is a serial dependent chain with nothing to overlap, so it costs
more than the copy it imitates. The ARM7 figure is inflated most, because its
duplicate also doubles a mispredictable branch.

**The ARM7 is where this concentrates.** The two CPUs cost the same in
absolute terms, but the ARM7 does it with a sixth as many accesses:

| | cost-model runs/frame | % of frame | host cycles per run |
|---|---|---|---|
| ARM9 | 72,194 | 2.61 | **10.0** |
| ARM7 | 11,336 | 2.63 | **64.2** |

**6.4x per access**, which is most of the 2.9x per-instruction gap between the
two CPUs. The cause is in the source: the ARM7 combine is a data-dependent
`cbnz` over up to fourteen instructions, and it cannot be a `csel` because the
guest NZCV live in the host NZCV.

#### Done: the ARM7 cost table (`mem::Timing::cost7`)

Every input to the ARM7 rule except the data page is known when a block is
translated -- `nc` (which takes exactly **four** distinct values, {1,2,8,9},
because `set_region7` only ever emits three timing tuples), `cdi`,
`code_main`, and the access width. So the model is evaluated once per page at
table-build time and the recompiler spends a load:

```
lsr  w6, w1, #15
add  x6, x15, x6, lsl #5
add  x6, x6, #COST7_OFFSET
ldrb w6, [x6, #slot]        ; slot = code_main*16 + cdi*8 + nc_idx*2 + word
...
sub  w8, w8, w6
```

Five instructions for three plus a mispredictable branch over up to fourteen.
Emitted code fell **1777 KB -> 1494 KB**, hot bytes per guest instruction
**38.4 -> 33.7**, from an ARM7-only change.

| scene | delta frame | SE | t | faster in |
|---|---|---|---|---|
| sm64 | **-1.31 %** | 0.16 % | 8.4 | 8/8 |
| mlbis | **-0.96 %** | 0.11 % | 8.6 | 8/8 |
| meteos | **-1.22 %** | 0.17 % | 7.4 | 6/6 |

Byte-identical frames on all five scenes against the unmodified baseline, and
the differential fuzzer green with the table on and off (`DS_JIT_NOCOST7`
keeps the A/B inside one binary).

Two notes for whoever extends this. **Block transfers still use the inline
model** -- their `nd` is `N + (n-1)S`, computed at run time, so it is not a
table input; that is 22 % of ARM7 combines. And the first version repointed the
pinned timing register at the cost table, which silently broke the two *other*
users of it (the ARM7 refill paths in `runtime.cpp`); the fuzzer caught it as a
7-cycle divergence. The table now sits *after* the raw one so nothing existing
moves. `set_region7` rebuilds the affected range itself, because EXMEMCNT
retimes the GBA slot long after reset and a stale entry would be a silent
timing divergence.

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

### F. LDM/STM — *struck 2026-08-25, the premise was wrong*

The audit put straddle probability at "~6 % for a random 8-register POP, and
stacks straddle more often than randomly". **Measured, it is 0.1-0.8 %**
(sm64 52 of 7,415 transfers a frame; mlbis 12 of 8,677). Stacks are aligned,
so they straddle *less* than randomly, not more -- the estimate assumed uniform
placement for the one case where placement is least uniform.

At ~483 cycles a fallback that is **0.16 % of frame**. Not worth doing, and
§E (0.85 %) should be ranked above it.

### G. The slow-access path — *the largest single item, and not on this list*

Measured 2026-08-25. `perf` on the emulation thread, summed across every
bucket a guest load or store that leaves the inline page-table path passes
through:

| component | % of frame |
|---|---|
| `Io::` register decode | 6.27 |
| `Gpu3D::write` + `gxfifo_write` (guest stores) | 2.68 |
| `Bus` / page table / timing | 2.60 |
| `call_pure` spill and reload | 1.89 |
| `jit_h_ld*` / `st*` helpers | 1.39 |
| slow load/store stubs | 0.52 |
| **total** | **15.36 %  =  2.13 ms/frame** |

That is **2.7x DraStic's entire CPU-emulation budget** (0.80 ms), across
13,245 accesses a frame -- **~320 host cycles each**, or ~210 excluding the
Gpu3D command-submission share.

It is invisible per-symbol because it is smeared over six buckets: exactly the
trap this document opens with, one level further down. The audit marked §4
"done" after the `Io::read` jump-table and stack-protector fixes, which were
worth 3.5 %; but what was fixed was the *decode*, and the path around it was
never priced.

**The traffic is extremely concentrated** (host census, `DS_CENSUS=1`; MMIO is
~99 % of slow accesses -- 13,435 counted against 13,245 measured):

| register | sm64 | mlbis |
|---|---|---|
| `0x040002B0` SQRTCNT | 31.2 % | 3.2 % |
| `0x04000280` DIVCNT | 28.2 % | 12.0 % |
| `0x040001A4` ROMCTRL | 7.2 % | **55.1 %** |
| top three | **66.6 %** | **73.5 %** |

All three are status-bit polls. Two independent attacks:

**(a) Cut the count.** Page-table granularity is 2 KB and the IO registers are
interleaved, so a shadow page cannot work. The tractable form is recognising
the address *at translate time* and emitting an inline read -- which needs
**constant tracking (§3 of the audit, where we have nothing)**. That makes §3
the enabler for the largest item rather than a standalone nicety. Note the
audit already tried short-circuiting the eight hottest addresses and it *lost*
to the general fix -- but that was inside the helper, after `call_pure` had
already been paid, which is a different proposition from translate time.

**(b) Cut the per-access cost.** ~320 cycles for a spill, a call chain and a
register decode is pathological. This is a
`disassemble-before-theorising` job on `call_pure` and `Io::read`, it needs no
new mechanism, and it is free -- host side, no device. **Start here.**

### On B, now that it has a denominator

5,981 indirect branches a frame (sm64; mlbis 9,417) against 0.31 ms of
dispatch and branch-indirect stubs is **~103 cycles each** -- an order above a
seven-instruction probe. The audit measured a LUT *miss* at ~830 cycles, so
roughly 12 % misses would account for the whole figure. **The first move is
measuring the miss rate, not shortening the probe**; the audit already proved
shortening buys nothing.

## Order of work

Ranked by expected value against the 4.11 ms, not by novelty. Every step gets a
byte-identical frame check and a **paired** benchmark (see the rig notes: pair
the interleaved reps, ~0.09 % resolution at 12 reps).

**Superseded 2026-08-25 by the measurements above.** The list below was ranked
before any of §A-§F had a number against the 4.11 ms. What replaced it:

1. ~~**Price §A.**~~ **Done** — 5.58 % of frame, and the ARM7 half of it is
   built and landed at -1.0 to -1.4 % across three scenes.
2. **§G(b), the slow-access path's per-access cost.** 15.4 % of frame at ~320
   cycles an access. Disassembly first; no new mechanism, no device needed.
3. **§G(a) via audit §3, constant tracking.** Three registers are two thirds of
   the traffic; catching them at translate time is the only way to cut the
   count.
4. **§B, but measure the LUT miss rate first.** ~103 cycles a dispatch is a
   miss-rate story, not a probe-length story.
5. **§A's ARM9 remainder.** Ceiling 2.62 %, but 55 % of §A is the load itself
   and no table removes a load — realistically ~1 % of frame.
6. **§E** (0.85 %), then **§D** if the branch-miss counters justify it.
7. ~~**§C**~~ and ~~**§F**~~ — struck, see above.

### The superseded ordering, for the record

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

## Build flags: the largest single win of the session, and it was not in the code

Found 2026-08-25 by running the disassembly census over every hot symbol
instead of picking targets by hand. The census itself found nothing
actionable -- see "what the sweep ruled out" below -- but it surfaced the
compile line, which was:

```
-O2 -g -DNDEBUG -std=c++17 -Wall -Wextra -Wno-unused-parameter
```

No `-O3`, no `-mcpu`, no LTO. Paired interleaved reps, `dsperate.fl-base`
against `dsperate.fl-o3` (`-O3 -mcpu=cortex-a55 -fno-stack-protector`), 1800
frames, 8 reps a scene:

| scene | base ms | O3 set ms | delta | t | slower in |
|---|---|---|---|---|---|
| sm64 | 13.650 | 12.842 | **-5.92 %** | -89.9 | 0/8 |
| mlbis | 15.099 | 14.442 | **-4.35 %** | -24.3 | 0/8 |
| meteos | 11.147 | 10.641 | **-4.53 %** | -31.5 | 0/8 |
| dbori | 20.902 | 19.281 | **-7.75 %** | -47.4 | 0/8 |
| etody | 10.905 | 10.234 | **-6.15 %** | -38.1 | 0/8 |

**Faster on every scene, 40 of 40 paired reps, frames byte-identical on all
five.** Four to five times the ARM7 cost table, for a build flag.

**The win is renderer-side** -- 2D select -8 %, 3D spans -5 %, geometry -9 %,
while CPU moves under 1 %. That is the expected shape: the recompiler emits
its own code, so compiler flags cannot reach the half of CPU that is
translated guest code. **It does not narrow the CPU gap; it widens CPU's
share of the frame**, which strengthens rather than weakens the case for
staying on CPU work.

### Attributing the three flags

The bundle above is three changes, so each was then measured against `-O3`
alone (paired, 6 reps, sm64 and dbori -- dbori has the largest effect and is
the most sensitive discriminator):

| flag added to `-O3` | sm64 | dbori | verdict |
|---|---|---|---|
| `-mcpu=cortex-a55` | -0.50 % (t -2.7, slower in 1/6) | -0.25 % (t -1.3, slower in **2/6**) | **not adopted** |
| `-fno-stack-protector` | -1.02 % (t -12.6, 0/6) | -2.43 % (t -11.1, 0/6) | superseded, below |
| **basic `-fstack-protector`** | **-1.07 %** (t -9.0, 0/6) | **-2.50 %** (t -10.9, 0/6) | **adopted** |

Two conclusions, both counter to what was expected going in.

**`-mcpu=cortex-a55` is worth ~0.25-0.5 % and is not adopted.** dbori does not
resolve at all. The in-order-scheduling argument for it -- the compiler's
schedule *is* the execution order on an A55, so telling it the pipeline should
matter -- was made twice and was wrong both times, once predicting a large win
and once reporting a regression from an under-powered sequential sweep. It is
not worth pinning the binary to one core.

**The stack protector cost is real, but it is not a safety trade.** The
toolchain default is `-fstack-protector-strong`, which guards **462** sites in
the core objects. Basic `-fstack-protector` keeps **76** -- the functions with
actual arrays, which is what a malicious ROM would target -- and recovers the
*entire* difference, measuring level with `-fno-stack-protector` (which guards
zero). So `-strong` was costing 1-2.5 % of frame to protect 386 functions that
have nothing to overflow. `add_compile_options(-fstack-protector)` is in
`CMakeLists.txt`; do not let it drift back to `-strong`.

This also resolves an open item in the notes: an earlier session tested the
guard on `render3d.cpp` alone, read +-0.1 %, and recorded it as never actually
resolved. It was too narrow -- the cost is an aggregate across every file, and
`render_polygon_line` is a 1884-instruction function where a guard is noise.
The small hot leaf kernels are where it is paid.

### What the sweep ruled out

Recorded so the same leads are not re-found:

- **The twelve 64-bit `sdiv`s in `render_polygon_line` are cold.** The region
  holding ten of them takes 3.7 % of that function's samples; they are
  GCC-hoisted fallbacks behind `b.eq <far> ... b <back>`. Checked by pulling
  the sample offsets out of `perf script -F ip,sym` and aligning them against
  a local disassembly, which is the only way to tell a hot instruction from a
  cold one in a 1884-instruction function.
- **`Io::read16`'s 98 compare-branches is a whole-function count, not a path
  length.** Tracing the real path for DIVCNT, GCC pivots on `0x040001A2` then
  `0x04000212` and hits it at the third value comparison: six compares. The
  `Io::read` pathology does not repeat here.
- **`-mcpu=cortex-a55` alone did not pay** in the first (sequential,
  under-powered) sweep, against the in-order-scheduling argument for it. Do
  not adopt it on that argument alone.

## Session total, 2026-08-25

The original binary against the tree at the end of the session, paired
interleaved, 1800 frames, 6 reps a scene:

| scene | before ms | after ms | delta | t | slower in |
|---|---|---|---|---|---|
| sm64 | 13.865 | 12.908 | **-6.91 %** | -92.6 | 0/6 |
| mlbis | 15.244 | 14.514 | **-4.79 %** | -24.5 | 0/6 |
| meteos | 11.354 | 10.632 | **-6.36 %** | -49.3 | 0/6 |
| dbori | 20.958 | 19.458 | **-7.16 %** | -77.9 | 0/6 |
| etody | 10.998 | 10.286 | **-6.47 %** | -29.9 | 0/6 |

**30 of 30 paired reps, frames byte-identical on all five scenes.** Made of
`-O3`, basic `-fstack-protector`, and the ARM7 cost table; `csel` is in the
tree but its own contribution is unmeasured. The parts are not quite additive
(5.7 + 1.8 + 1.2 would be 8.7 %), which is expected where they touch the same
code.

**Almost none of it is CPU.** Two of the three are build flags whose win is
renderer-side, and the recompiler emits its own code where compiler flags
cannot reach. The CPU emulation is very close to where this document found it,
and now occupies a *larger* share of a smaller frame. The 5.2x gap against
DraStic stands, and the ranked leads in "Order of work" are unchanged.

## What success looks like

- CPU stage time per frame, `DS_PROFILE=1`, direct-boot SM64DS 300 frames,
  against the 4.11 ms baseline here.
- Cycles per guest instruction, against 23.4 now and 4.6 for DraStic.
- Whole-frame ms against 8.28, compared to DraStic's 2.98 on the identical
  workload — the only comparison that is not scale-free.

Halving the CPU gap takes the frame to ~6.2 ms. Reaching DraStic's CPU cost
takes it to ~5.0 ms, at which point the renderer becomes the majority again and
the parked [span-batching branch](../README.md) is worth revisiting.
