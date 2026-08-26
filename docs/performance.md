# Where the time goes

One document, reconciled 2026-08-26. It replaces `profile-vs-drastic.md`,
`plan-cpu.md`, `plan-spu.md` and `plan-render3d-binning.md`, whose premises
this round's measurements invalidated. `techniques/` and `material/` are kept:
they are source material about the DS and about DraStic, not conclusions about
us.

Everything here is measured on the RK3566 handhelds against the five replay
scenes (`scenes/README.md`). Where a number is an estimate or a fit, it says so.

---

## 1. The headline

Against DraStic on a matched SM64DS gameplay replay, emulation code only:

| metric | ours | DraStic | ratio |
|---|---|---|---|
| CPU-time per frame | 18.87 ms | 4.38 ms | **4.31x** |
| instructions per frame | 24.66 M | ~5.5 M | **~4.5x** |
| IPC | 0.688 | 0.635 | 1.08x *(ours better)* |
| L1D refills per 1k instructions | 4.75 | 4.53 | 1.05x *(ours worse)* |

**The gap is instruction volume at per-instruction parity.** The time ratio and
the instruction ratio are the same number, so there is nothing left over for
stalls, cache layout or data-structure size to explain. Two independent routes
agree on the volume figure: `time_ratio x IPC_ratio` = 4.67x, and their implied
frame count from the established 4.38 ms/frame gives 4.45x.

This kills the cache/memory hypothesis as a *global* explanation. We are not
slower per unit of work; we do about four and a half times the units.

It also explains why the two largest wins on this project are both build flags
(`-O3`, then LTO) and why deleting the 2D compares pays: all three remove
instructions.

### Where the instructions are

| group | ours M/frame | theirs M/frame | ratio | **excess** | share of gap |
|---|---|---|---|---|---|
| **3D raster** | 12.19 | 2.28 | 5.35x | **9.91** | **51.5 %** |
| CPU + memory + IO + sched | 5.94 | 0.88 | **6.75x** | 5.06 | 26.3 % |
| 2D | 3.64 | 1.35 | 2.70x | 2.29 | 11.9 % |
| 3D geometry | 2.06 | 0.58 | 3.55x | 1.48 | 7.7 % |
| SPU | 0.79 | 0.29 | 2.72x | 0.50 | 2.6 % |

CPU/memory/IO/scheduler are grouped because **their boundaries are not
comparable across the two codebases** — DraStic's memory and IO helpers
(`arm64_store_memory32_arm9`, `store_io_register_arm9_32`) live in its main
binary, ours are split across `mem/`, `io/` and the JIT stubs. Split apart they
produce nonsense like "scheduler 75x" off a 0.01 M/frame denominator.

The DSO shape rules out the obvious suspect: ours is 85.6 % own code / 11.5 %
translated guest code, theirs is 87.2 % / 9.7 %. This is not a bloated JIT. The
excess is spread across our own C++, which is exactly the uniform-across-
subsystems signature that made this hard to find.

### The headline is measured against a contaminated DraStic run — re-measure it

Both numbers in the table above come from `perf stat` on
`drastic-sym --benchmark 300 --input-playback sm64ds`, and **that invocation is
wrong twice over**: the bare name fails to open the recording (DraStic then
free-runs the ROM from direct boot, rc=0), and `--benchmark` replays from
savestates that do not exist. §6 records that using either inflated the
instruction ratio by ~50 %.

An exact host-side count (2026-08-26, qemu + `libhotblocks`, per-symbol, both
emulators single-threaded, DraStic's frameskip disabled and libSDL2 excluded)
puts the core ratio at **3.04x**, not 4.5x — and 4.5 / 1.5 = 3.0, exactly the
inflation §6 predicts. **Treat 4.31x and 5.5 M/frame as unverified** until the
device run is repeated with `--input-playback input_record/<name>.ir` and no
`--benchmark`.

### Exact counts, per symbol (qemu, 2026-08-26)

Ours: sm64 scene, 1800 frames, `DS_R3D_THREADS=1`. DraStic: its own 2143-frame
recording, `threaded_3d = 0`, `frameskip_type = 0`. Not the same content, so
read the ratios as indicative and the per-polygon figures below as the real
result. Reproduces to 1,021 instructions in 39.5 billion.

| subsystem | ours /frame | DraStic /frame | ratio |
|---|---|---|---|
| 3D raster | 9,647,233 | 3,081,820 | 3.13x |
| 2D render | 3,496,482 | 1,373,218 | 2.55x |
| CPU / JIT | 3,392,746 | 1,149,877 | 2.95x |
| MMIO | 1,767,032 | 320,532 | **5.51x** |
| 3D geometry | 1,524,826 | 734,217 | 2.08x |
| SPU | 744,968 | 229,871 | 3.24x |
| DMA / timers | 562,326 | 100,079 | **5.62x** |
| **core total** | **21,968,886** | **7,225,657** | **3.04x** |

**libSDL2 is 49.3 % of DraStic's process** even with `SDL_VIDEODRIVER=dummy`,
and a single unexported function at `+0xc1de8` is 37 % of everything — 5.27 M
instructions per frame. Our CLI has no display path, so it is excluded above;
never quote a DraStic total without saying which side of that line it falls on.

Two cross-checks that the instrument is sound: qemu counted exactly 5,241,970
calls to `render_polygon_line`, and `DS_PROFILE=1` counts exactly 5,241,970
polygon lines; and the implied 32.3 px mean span matches the 31.6 px measured
on device.

---|

---

## 2. Inside the 3D raster — half the gap

Instructions per frame by stage (sm64):

| stage | ours | theirs | ratio |
|---|---|---|---|
| **shade/blend + writeback** | 5.76 | 0.508 | **11.4x** |
| **polygon driver / edges** | 2.96 | 0.166 | **17.9x** |
| depth load/test | 0.96 | 0.223 | 4.3x |
| writeback/final (`final_pass`) | 0.72 | — | n/a |
| perspective setup | 0.82 | 0.284 | 2.9x |
| interpolate + interpolant setup | 0.84 | 0.583 | 1.4x |
| **texel fetch** | 0.06 | 0.131 | **0.44x — we win** |

Normalised to work units:

- **shade: 54.3 instructions per resolved pixel, against DraStic's 5.2.**
- **driver: 842 instructions per polygon line, against DraStic's 50.7.**

> **This overturns a long-standing position on this project.** The old
> `profile-vs-drastic.md` concluded "our intrinsics already beat their
> assembly — stop proposing work on the pixel stages". That was built on
> *shares of each emulator's own code* (16.4 % ours vs 26.8 % theirs), which is
> scale-free and says nothing about absolute cost. In instructions per unit of
> work we are 10-18x worse. Only texel fetch survives that correction.

### Two distinct causes, separated by census

**(a) The driver cost is fixed per scanline.** Instructions per polygon line
across the five scenes: 700 (dbori), 705 (meteos), 735 (etody), 767 (mlbis),
842 (sm64). A controlled two-scene comparison at 4x sample density is stronger
still: sm64 (31.6 px mean span) **617.4** against mlbis (12.7 px) **603.1** —
**0.98x**. Halve the span length and the per-scanline cost does not move. It is
per-scanline re-derivation, not per-pixel work, so hoisting it to per-polygon
removes it outright rather than redistributing it.

**(b) The shade cost is a per-span preamble.** Instructions per resolved pixel
is monotone in mean span: mlbis 92.8, dbori 79.5, meteos 70.4, sm64 54.3,
etody 44.9. A least-squares fit gives **~816 instructions fixed per span plus
~30 per pixel** — good for mlbis/dbori/sm64, ~15 % off for meteos/etody, so
directional rather than exact. DraStic shows no per-span term at all.

### Why (b) bites: the mean span lies

Span-length histogram (`C_SL0..6`, `C_SLPX0..6`):

| scene | mean span | **spans ≤ 8 px** | pixels in spans ≤ 8 |
|---|---|---|---|
| sm64 | 31.6 | **59.9 %** | 8.4 % |
| dbori | 16.7 | 57.6 % | 12.1 % |
| mlbis | 12.7 | 54.3 % | 28.3 % |
| etody | 35.5 | 37.2 % | 4.0 % |
| meteos | 28.4 | 0.6 % | 0.1 % |

sm64's distribution is **bimodal**: a 31.6 px mean, yet 60 % of spans are ≤8 px
and 74 % of its span *pixels* are in 129-256 px spans. Use the histogram, never
the mean. An ~816-instruction preamble is being paid, most of the time, on
spans of eight pixels or fewer.

---

## 3. The structural difference

Read out of the Ghidra decompilation in
`dsperate-research/ghidra_export/decomp` — 2677 named functions, unused for
most of this project's life.

`render_polygon_setup_1x` (0014b110) is 69 lines and is DraStic's entire
scanline loop: it walks a per-scanline span-length array built once per
polygon, **accumulates consecutive scanlines until the running pixel total
would exceed 257**, then calls `render_polygon_flush_1x` once for that batch.
`flush_1x` calls **each stage kernel exactly once** — perspective steps,
interpolate z/w/rgb/uv, texture addresses, load texels, depth compare, fog,
edge marking, alpha pass/blend/id/combine.

| level | DraStic | ours |
|---|---|---|
| per polygon | `setup_spans_asm_1x` (every scanline's span at once), `interpolate_edges`, `setup_edge_markers` | `build_edges` |
| per ≤256-px batch | `flush_1x` → every pixel stage, once each | `flush_batch` → `span_texels`, `span_shade` once each |
| per scanline | pointer bump + compare against 256 | **`render_polygon_line`, ~600-840 instructions** |
| per span | — | **`resolve_one` ×2.4-2.9 kernel calls** |
| per pixel | inside the asm kernels | inside |

**Our accumulator is not the problem.** `BATCH_PX` is already 256 and the batch
already crosses scanlines within a polygon; measured fill is 95-251 px. DraStic's
`setup_1x` is also per-polygon and also does not cross polygon boundaries. The
difference is the two levels we have that they do not.

**The confirming evidence is internal to our own renderer**: the one stage we
already batch — texel fetch — is the one stage we beat them on (0.44x). Same
kernels, same NEON, same code. The only variable is whether the setup is
amortised.

Also worth knowing: DraStic has **306 `render_*` functions**, with a specialised
variant per mode combination (`alpha_combine_depth_fog_constant_asm`,
`generate_texture_addresses_clamp_flip_asm`). Mode is resolved by selecting a
kernel at setup time, never branched on per pixel — the same "decide early"
pattern as the batching, applied to control flow.

### The per-scanline cost, itemised

There is no single dominant item; it is roughly ten items of 20-90 instructions
each, which is why it reads as a flat tail:

| | instructions / scanline (sm64) |
|---|---|
| `Interp::interpolate` ×14 | ~69 |
| staging in `render_polygon_line` | ~90 |
| `flush_batch` NEON *(legitimate pixel work)* | 77 |
| `setup_left_edge` / `setup_right_edge` | ~48 |
| `build_edges` | ~35 |
| `resolve_one` walk | ~28 |
| `render_chunk` | ~17 |

**So this is a campaign, not a refactor.** The change that subsumes the most of
it is DraStic's structure: precompute every scanline's span endpoints and edge
attributes once per polygon, leaving the scanline loop as pointer arithmetic.

### Priced exactly: 11.7x on per-polygon setup

The qemu run settles what the per-scanline structure actually costs, because
DraStic's edge and span setup is all per-polygon and can simply be divided by
its polygon count (`render_polygon_setup_1x` and `render_polygon_setup_spans_asm_1x`
agree exactly, 1,088,143 calls).

| DraStic, per polygon | instr |
|---|---|
| `render_polygon_edge_interpolate_x_c` | 191.1 |
| `render_polygon_edge_interpolate_parameters_asm` | 183.3 |
| `render_polygon_setup_1x` | 128.7 |
| `render_polygon_interpolate_edges` (2 clones) | 219.4 |
| `render_polygon_setup_spans_asm_1x` | 107.9 |
| `render_polygon_edge_perspective_coefficients_asm` | 99.2 |
| `render_polygon_edge_interpolate_w_asm` | 81.4 |
| `render_polygon_edge_perspective_steps_asm` | 53.5 |
| **total** | **1,064.5** |

Ours, the same job, done once per scanline instead of once per polygon:
`render_polygon_line` **841.5** + `span_stage` **239.4** = 1,081 per scanline,
and sm64 averages **11.5 scanlines per polygon** (5,241,970 lines / 455,437
polygons) — **12,441 instructions per polygon, 11.7x DraStic's 1,064.5.**

At 455,437 polygons over 1800 frames that is **2.88 M instructions per frame,
13.1 % of our 21.97 M total**, and it is the ceiling on commit 3 of the
refactor plan. Not all of it is removable — the depth pre-pass and the parts of
`span_stage` that depend on live `depth_`/`attr_` cannot be hoisted — but the
interpolation and edge setup can.

**This supersedes the "842 vs 50.7 per line, 16.6x" figure** used to size this
work earlier. 50.7 counted only part of DraStic's setup; the full per-polygon
total divided by our 11.5 scanlines is ~93 per line, so the true ratio is
11.7x, not 16.6x. Bigger than any win taken this round by an order of
magnitude, but not as large as advertised.

**Corroborating the same structure from the other end:** DraStic runs its pixel
kernels ~1.15 times per polygon (`render_polygon_shade`,
`render_polygon_interpolate_uv_asm`) — a whole polygon is usually one batch. We
call `flush_batch` **3.64 times per polygon**.

### `resolve_one`, sized

`resolve_one` splits each span into three parts — left edge run, interior,
right edge run — each an indirect call through `Shade::resolve`. Censused at
**2.35 (dbori) to 2.93 (meteos) calls per span**; sm64 makes 8,282 calls a
frame against 3,247 spans and 795 batches, so one call per batch would be ~10x
fewer invocations. Most of that kernel's preamble (`v_alpha_ref`, `v31`, `v0`,
`t_attr_fixed`, `t_keep`, `t_id`, `fogbit`, `blend_on`) is batch-invariant and
rebuilt every call; only `attr_base`/`cov_*` (per part) and `row0` (per span)
vary. Batching it properly needs DraStic's approach: per-pixel edge/part flags
written during staging so the writeback is one flat pass. The open design
problem is `row0` — each span writes a different framebuffer row.

---

## 4. The other subsystems

### CPU — 26 % of the gap, worst ratio (6.75x)

**23.4 host cycles per guest instruction against DraStic's 4.6**, and
`jit-technique-audit.md` had that number long before it was believed. Bucketed
share of CPU: translated guest code 50.6 % (sm64) / 56.3 % (meteos), JIT stubs
16.1 %, IO registers 14.8 %, memory 6.1 %.

- **The ARM7 is the concentrated inefficiency**: 34 % of translated-code time
  for 15 % of guest instructions — **2.9x per instruction**, and **6.4x** on the
  per-access cycle accounting, because its combine is a data-dependent `cbnz`
  over up to fourteen instructions and cannot be a `csel` (the guest NZCV live
  in the host NZCV).
- **Per-access cycle accounting is priced at 5.58 % of frame** (SE 0.13, t=42,
  12/12) via `DS_JIT_COSTPROBE`. Read as a ceiling — but see the duplication
  caveat in §6.
- **Ruled out on measurement**: CPU switches (the scheduler is 3.2 % of the
  emulation thread and `--quantum 0` is already its good configuration), and
  `csel` for conditional execution (10,700 csel-able instructions a frame is
  5.0 % of the stream, so ~1 % of CPU even if every one saved a full mispredict).
- **Denominators come from the host**: `DS_CENSUS=1` under `--interp` counts the
  executed stream by shape. sm64/frame: 212,538 guest instructions, 83,530
  cost-model runs, 65 % in the same 4 KB page as the previous run, only 5.7 %
  pc-relative — which killed the "resolve the page statically" idea before any
  device time was spent on it.

### 2D — 11.9 % of the gap, best ratio (2.70x)

The five `memcmp` sites in `engine2d.cpp` ask "did anything change" by
rescanning, and the `_checked_` flags are cleared **per scanline**, so each runs
384x a frame per engine. Censused per frame: 325 KB (etody), 375 (dbori), 489
(mlbis), 550 (sm64), 786 (meteos), at a differ rate of **0.001 % to 0.11 %**.
Over 99.9 % of that scanning finds nothing.

**Priced both ways** (§6 explains why the two disagree):

| scene | `DS_2D_CMPPROBE` (duplicate) | `DS_2D_CMPFRAME` (remove) |
|---|---|---|
| meteos | +2.42 % | **−7.52 %** |
| sm64 | +1.44 % | −2.96 % |
| mlbis | +1.18 % | −2.82 % |
| etody | +1.05 % | −1.65 % |
| dbori | +0.28 % *(t=1.5, unresolved)* | −1.91 % |

### SPU — 2.6 % of the gap

546 output samples a frame, each mixing 16 channels — 8,738 per-channel
operations with nothing amortised, the same shape as the renderer's per-span
problem. DraStic works in blocks: `spu_render_samples` (plural),
`spu_adpcm_decode_block`, `spu_clamp_block`. Small share of the gap; the fix is
the same idea as the renderer's, so it will be cheaper to do *after* that work
than before it.

---

## 5. Settled — wins taken, and dead ends

**Taken:**

| change | effect |
|---|---|
| `-O3` (was `-O2`) | −4.35 % to −7.75 %, 40/40 reps |
| basic `-fstack-protector` (was `-strong`) | −1.07 % (sm64) to −2.50 % (dbori) |
| **LTO** | −0.65 % to −2.97 %, 39/40 reps, binary 625 KB smaller |
| `DS_R3D_SKIPDUP` (gated) | −0.66 % to −1.33 % on three scenes, null on two |
| `-DDS_R3D_RING=8` (tile 99 KB → ~50 KB) | −0.30 % to −1.64 % on all five |

**Dead ends, measured:**

- **Loosening the 3D register comparison** — it rejects 0-13 frames per 1800.
  Worth nothing.
- **Removing the untextured immediate flush** (`DS_R3D_KEEPUNTEX`) — moves batch
  fill 232→240 px on sm64, nothing elsewhere.
- **`-mcpu=cortex-a55`** — 0.25-0.5 %, and dbori does not resolve.
- **Silent-store elimination**, **static page resolution for the cost model**,
  **1024-entry branch LUT** (costs 1.6-2.7 % against 64 K; 8 K is break-even).
- **The stack-protector guard and the 8 KB frame in `render_polygon_line`** —
  both looked like the `Io::read` pattern, both measured as nothing.

**Designed, priced, pinned:**

The **2D write-path dirty bit**. Not a quick change: palette and OAM are mapped
RW straight into the JIT page table (`bus.cpp` ~98-99), so the recompiler stores
to them in one instruction with no notification — there is no write path to
hook, which is *why* the memcmp exists. The hook that does exist is
`TAG_SPECIAL` (page_table.h bit 62), which diverts stores to the slow path;
using it means adding palette/OAM handling to `io_write`.

The census says do it anyway. Stores per frame (CPU *and* DMA — both go through
`PageTable::write_ptr`, so this is complete):

| scene | palette stores | OAM stores | compares |
|---|---|---|---|
| sm64 | **2** | 374 | 776 |
| mlbis | **3** | 425 | 743 |
| meteos | **2** | 381 | 1275 |
| dbori | **3** | 56 | 567 |
| etody | **2** | 121 | 517 |

Two to three palette writes a frame against 384+ compares scanning 200-500 KB.
The ceiling is already measured (`DS_2D_CMPFRAME`, above).

---

## 6. Method — the traps, each of which cost a wrong conclusion

**Measure on the device, and pair the reps.** Interleave A/B/A/B and compute
`mean(delta_i)`; 12 paired reps gave SE 0.09 % where 3 unpaired reps had a
0.5 % floor. **A null from an unpaired handful is under-powered, not null** —
that error mis-called both the stack protector and LTO, and LTO turned out to
be worth up to 3 %.

**Prove semantics unchanged** with `--dump-frames` and `cmp`. Byte-identical is
the bar. A speedup that changes output is measuring a different workload.

**A share is not an absolute.** Comparing shares of each emulator's own code
made us "win" on pixel kernels while being 4.5x slower there. Compare
instructions or milliseconds per unit of work.

**A share of frames is not a share of work.** The duplicate-swap skip looked
like 2.8 ms/frame from frame counts, then like nothing from polygon counts,
before span pixels and polygon lines gave the real answer. Three sizings, two
wrong.

**Duplication UNDERestimates memory-bound costs.** `DS_2D_CMPPROBE` (run it
twice) priced the 2D compares at +1.4 %; removing them was worth −3.0 % on the
same device, and the gap held on all five scenes. The duplicate re-reads L1-hot
bytes and evicts nothing, so it prices the *cheapest* pass; removal recovers the
one that drags 1.5 KB per scanline through a 32 KB L1D. The "duplication
overestimates" argument is about instruction scheduling and applies only to
ALU-bound costs.

**A tool that reports per-symbol counts beats one that reports shares.**
qemu-user's `libhotblocks` (patched for an unbounded dump and a periodic
snapshot) gives exact instructions *and* call counts per function for both
emulators — no sampling, reproducible to 1,021 instructions in 39.5 billion.
Everything in §1 that used to be a share is now a count. Three traps in
building it, each of which silently produced a wrong answer first:

- **qemu maps guest images `r--p`, not `r-xp`** — their pages are translated,
  never executed natively. Filtering the guest's `/proc/<pid>/maps` on the exec
  bit attributes 100 % of samples to "unknown".
- **Snapshot the map late, or repeatedly.** A JIT's code buffer is mapped after
  startup, so a map taken at t=1s loses every instruction the recompiler
  emitted. Anything not file-backed *is* translated code; bucket it as such.
- **Disable the other emulator's frameskip.** DraStic paces against wall-clock
  and qemu is ~50x slower than real time, so it sits past `BEHIND_THRESH` and
  drops nearly every *video* frame while the emulated frame count stays
  identical. Nothing in the output reveals it. See [drastic-under-qemu].

**Never edit a shell script while bash is running it.** `sed -i` on
`drablocks.sh` mid-run made bash resume at a shifted byte offset and re-execute
the launch line, so two DraStic runs wrote to one output file — the same
double-run contamination as the earlier `.25` sweep, from a new cause.

**Match the workload, and check the tool actually did what you asked.**
DraStic's `--input-playback` takes a **path including the `.ir` extension**
(`input_record/sm64ds.ir`); given a bare name it prints `Couldn't open ... for
input playback.` **and carries on running the ROM free from direct boot** —
rc=0, no failure. Separately, `--benchmark` loads per-phase savestates that do
not exist, so it runs direct-boot ablations across 7 phases. Using either by
mistake inflated our measured instruction ratio by ~50 %. Grep every run for
`couldn.t open`. Plain `--input-playback <path>.ir` terminates on its own.

**libSDL2's share depends on the mode**: 50.6 % of DraStic's process
instructions under playback, 0.37 % under `--benchmark`. Renormalise per run,
never from memory.

**Profile by instructions when the question is volume.** Every ranking on this
project was cycle- or sample-based, which cannot separate "hot because it
stalls" from "hot because there is a lot of it". Against a 4.5x volume gap,
`perf record -e instructions` is the ranking that matters. Note `addr2line`
needs `-i` for inline-aware attribution or NEON intrinsics are credited to the
header and 27 % of samples fall out of the mapping.

**Check the build line before profiling anything**
(`grep 'FLAGS = ' build/*/build.ninja`). It is not per-symbol, not in any
profile, and not in any technique document, so nothing in the normal workflow
points at it — and it has twice been the largest win available.

---

## 7. Next

1. **Per-polygon precompute of span endpoints and edge attributes.** Subsumes
   the interpolation (~69/line), edge setup (~48/line) and part of staging.
   Biggest lever; interacts with the chunked/threaded band structure, since each
   band worker would precompute its own slice.
2. **Batch `resolve_one`** via per-pixel edge flags — ~10x fewer kernel calls.
3. **Hoist the batch-invariant kernel preamble** into a context struct — small
   (~1-3 %), mechanical, and quick to measure.
4. **The 2D dirty bit**, when its slow-path work is worth scheduling.
