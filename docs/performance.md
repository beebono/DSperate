# Where the time goes

One document, reconciled 2026-08-26. It replaces `profile-vs-drastic.md`,
`plan-cpu.md`, `plan-spu.md` and `plan-render3d-binning.md`, whose premises
this round's measurements invalidated. `techniques/` and `material/` are kept:
they are source material about the DS and about DraStic, not conclusions about
us.

Everything here is measured on the RK3566 handhelds against the five replay
scenes (`scenes/README.md`). Where a number is an estimate or a fit, it says so.

**Status, 2026-08-26:** the gap is **2.94x in instructions per frame, emulation
code against emulation code**, and the device and qemu instruments now agree to
within 5 % on both sides — see §1. The plan is a single rewrite that deletes the
rasteriser's per-scanline level; §7.

---

## 1. The headline

Against DraStic on a matched SM64DS gameplay replay, emulation code only:

| metric | ours | DraStic | ratio |
|---|---|---|---|
| **instructions per frame, emulation code only** | **22.29 M** | **7.58 M** | **2.94x** |
| instructions per frame, whole process | 23.00 M | 14.93 M | 1.54x |
| emulation share of process instructions | 96.9 % | **50.8 %** | |
| IPC (whole process) | 0.689 | 0.696 | 0.99x *(theirs marginally better)* |
| CPU-time per frame, emulation (derived) | 16.7 ms | 5.5 ms | 3.03x |

Device, `/storage/dsperate/pmu2.sh` on 192.168.1.20, 2026-08-26: three
interleaved reps of a HEAD build against DraStic's 2143-frame
`input_record/sm64scene.ir`, plus a `perf record -e instructions` DSO split on
both sides. Our instruction count reproduces to six parts in 10^5 across reps.

**The gap is instruction volume at per-instruction parity.** The derived time
ratio (3.03x) and the counted instruction ratio (2.94x) are the same number, so
there is nothing left over for stalls, cache layout or data-structure size to
explain. IPC is within 1 %.

This kills the cache/memory hypothesis as a *global* explanation. We are not
slower per unit of work; we do about three times the units.

It also explains why the two largest wins on this project are both build flags
(`-O3`, then LTO) and why deleting the 2D compares pays: all three remove
instructions.

For context on the whole-system figure: our CLI has no display or audio output
path, and DraStic's is half its process. Emulation-against-emulation is the
honest comparison for deciding what to optimise, but a real frontend would put
that cost on us too, and the whole-process ratio is only 1.54x.

**Where we actually stand on this rig:** 11.07 ms mean wall-clock per frame,
**276-282 of 1800 frames over the 16.715 ms DS budget (15.3-15.7 %)**, against
a DraStic that frame-caps at 59 fps with headroom to spare.

### Where the instructions are

Ranked by **excess instructions per frame** — ours minus theirs, not a ratio —
off the exact per-symbol counts below. Total excess is 14.74 M/frame.

| group | ours /frame | theirs /frame | ratio | **excess** | **share of gap** |
|---|---|---|---|---|---|
| **3D raster** | 9,647,233 | 3,081,820 | 3.13x | **6.57 M** | **44.5 %** |
| CPU / JIT | 3,392,746 | 1,149,877 | 2.95x | 2.24 M | 15.2 % |
| 2D | 3,496,482 | 1,373,218 | 2.55x | 2.12 M | 14.4 % |
| MMIO | 1,767,032 | 320,532 | **5.51x** | 1.45 M | 9.8 % |
| 3D geometry | 1,524,826 | 734,217 | 2.08x | 0.79 M | 5.4 % |
| SPU | 744,968 | 229,871 | 3.24x | 0.52 M | 3.5 % |
| DMA / timers | 562,326 | 100,079 | **5.62x** | 0.46 M | 3.1 % |
| unbucketed | 833,273 | 236,043 | — | 0.60 M | 4.1 % |

**Rank by excess, never by ratio.** MMIO and DMA have the worst ratios on the
page and are together 12.9 % of the gap; the 3D raster has a middling 3.13x and
is 44.5 % of it. A large ratio on a small denominator is not a lever.

Memory and IO boundaries are **not comparable across the two codebases** —
DraStic's helpers (`arm64_store_memory32_arm9`, `store_io_register_arm9_32`)
live in its main binary, ours are split across `mem/`, `io/` and the JIT stubs.
Read CPU/JIT, MMIO and DMA as one 28 % block rather than three lines.

The DSO shape rules out the obvious suspect: of emulation code, ours is 87.2 %
own C++ / 12.8 % translated guest code, theirs 92.1 % / 7.9 %. This is not a
bloated JIT. The excess is spread across our own C++, which is exactly the
uniform-across-subsystems signature that made this hard to find.

### SETTLED: the device and qemu now agree, at parity

This page used to carry two irreconcilable figures — a device-measured **4.5x**
and a qemu-counted **3.04x**. Re-run on device 2026-08-26 with the corrected
invocation and a DSO split, they agree:

| instructions/frame, emulation only | device (`pmu2.sh`) | qemu (`libhotblocks`) | agreement |
|---|---|---|---|
| ours | 22.29 M | 21.97 M | +1.5 % |
| DraStic | 7.58 M | 7.23 M | +4.9 % |
| **ratio** | **2.94x** | **3.04x** | |

Two instruments, two different recordings, two different thread configurations,
both landing at ~3x. **Quote 2.94x. The 4.5x and the 4.31x were wrong** — their
5.5 M/frame was *implied* from IPC and cycle ratios, never counted, and one of
the two routes that "independently" agreed (`ratio_time x ratio_IPC`) reused
the time ratio it was checking.

**The DSO split is not optional on device, and `perf stat` alone is useless
here.** libSDL2 is **47.3 %** of DraStic's process instructions on device —
close enough to the 49.3 % qemu found to confirm it is real per-frame work and
not a wall-clock spin, even though DraStic frame-caps at 59 fps on this rig.
Without splitting it out, `perf stat` reports 1.54x.

**One claim did not survive and is now open.** The old "L1D refills per 1k
instructions: 4.75 vs 4.53, within 5 %" is not supported. Whole-process, the
device measures **4.92 (ours) against 2.96 (theirs)** — but theirs is diluted
by SDL and `perf stat` cannot attribute cache events per DSO, so this instrument
cannot settle it either way. L2D refills go the other direction (2.14 vs 3.08).
IPC parity holds regardless, so memory is still not the story; the specific
L1D number just should not be quoted until something can attribute it per DSO.

`/storage/dsperate/pmu2.sh` on 192.168.1.20 is the script: three interleaved
reps, abort on `couldn't open`, emulated-frame count reported, DSO split on both
sides. It supersedes `pmu.sh`, which took one unpaired rep, pointed at a stale
binary (`dsperate.cmp`, four code commits behind) and had no DSO split.

### Exact counts, per symbol (qemu, 2026-08-26)

**These are the numbers to work from.** They are exact per-symbol counts, and
the device run above independently confirms both totals to within 5 %, so the
subsystem split below can be trusted even though the two recordings differ.

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

**A third, from the device.** Ranking our own process by instructions retired
(`perf record -e instructions`, sm64, 1800 frames) puts the per-scanline level
at the top of the whole emulator:

| symbol | % of all our instructions |
|---|---|
| `render_polygon_line` | **10.72 %** |
| `resolve_batch_vec<1, true, true>` | 6.98 % |
| `Engine2D::render_line` | 4.76 % |
| `Engine2D::draw_bg_text` | 4.51 % |
| `resolve_span<1, true, true, false>` | 4.13 % |
| `resolve_batch_vec<1, false, true>` | 4.07 % |
| `span_stage` | 3.31 % |
| `flush_batch` | 2.11 % |
| `build_edges` | 1.49 % |

DraStic's own hot list on the same instrument is the mirror image: after the
libSDL2 cluster, `render_scanline_tiled_span_4bpp_asm` (1.48 %),
`render_polygon_load_depth_colors_id_asm_1x` (1.27 %), `render_polygon_shade`
(1.22 %), `render_polygon_interpolate_uv_asm` (1.05 %) — pixel kernels, no
per-scanline driver anywhere near the top.

---

## 2. Inside the 3D raster — 44.5 % of the gap

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

### CPU / JIT — 15.2 % of the gap (2.95x); with MMIO and DMA, 28 %

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

### 2D — 14.4 % of the gap (2.55x)

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

### SPU — 3.5 % of the gap

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
made us "win" on pixel kernels while being 10x slower per pixel. Compare
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

**The one DraStic invocation that is correct** — everything else on this page
depends on it, and it has now gone wrong twice:

```
cd /storage/drabench
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./drastic-sym --input-playback input_record/sm64scene.ir "/roms/nds/Super Mario 64 DS.nds"
```

- **The path needs the directory and the extension.** Given a bare name DraStic
  prints `Couldn't open ... for input playback.` and **carries on running the
  ROM free from direct boot** — rc=0, no failure, wrong workload.
- **Never `--benchmark`.** It loads per-phase savestates that do not exist, so
  it runs direct-boot ablations across 7 phases instead of the replay.
- Either mistake inflates the measured instruction ratio by ~50 %. **Grep every
  run for `couldn.t open`** — `/storage/dsperate/pmu.sh` now aborts on it.
- **`grep -c '^vf ticks'` is the exact emulated frame count**
  (`system_frame_sync` prints it unconditionally, once per frame). Check it
  against the recording: the last record’s frame number is where playback ends.
- Set `frameskip_type = 0` in `config/drastic.cfg` for any measured run. It
  matters enormously off-device (see the qemu trap above) and costs nothing on
  device.
- Plain `--input-playback <path>.ir` terminates on its own.

Recordings are 10-byte records (u32 frame, u32 buttons, u8 touch x, u8 touch y).
`input_record/sm64ds.ir` is `sm64.rec` renamed and runs **4205** frames from
boot; `sm64scene.ir` is the 230-byte `sm64ds.rec` and runs **2143**, which is
the one to use against our 1800-frame scene.

**libSDL2's share depends on the mode**: 50.6 % of DraStic's process
instructions under playback, 0.37 % under `--benchmark`. Renormalise per run,
never from memory.

**Profile by instructions when the question is volume.** Every ranking on this
project was cycle- or sample-based, which cannot separate "hot because it
stalls" from "hot because there is a lot of it". Against a ~3x volume gap,
`perf record -e instructions` is the ranking that matters. Note `addr2line`
needs `-i` for inline-aware attribution or NEON intrinsics are credited to the
header and 27 % of samples fall out of the mapping.

**Check the build line before profiling anything**
(`grep 'FLAGS = ' build/*/build.ninja`). It is not per-symbol, not in any
profile, and not in any technique document, so nothing in the normal workflow
points at it — and it has twice been the largest win available.

**Check which binary the measurement script points at.** `pmu.sh` still named
`dsperate.cmp`, four code commits behind HEAD, long after those commits landed.
A benchmark harness pinned to a filename silently measures last week's code.

**Split the other process by DSO before comparing totals.** Half of DraStic's
instructions are libSDL2, and our CLI has no counterpart. Comparing whole
processes gives 1.54x; comparing emulation to emulation gives 2.94x. Both are
true statements about different questions — say which one you are answering.

---

## 7. The plan: delete the per-scanline level

This list used to be four independent items. Items 1, 2 and 3 are now **one
rewrite**, because each is blocked by the same thing — the existence of a
per-scanline level in the rasteriser — and item 3 alone is worth 1-3 %. Doing
them separately means paying the integration cost three times and landing two
intermediate states that measure as noise.

### Why this and nothing else

| | excess/frame | share of the 14.74 M gap |
|---|---|---|
| 3D raster, **total** | 6.57 M | 44.5 % |
| — of which, per-polygon setup done per scanline | **2.88 M** | 19.5 % |
| — of which, the per-span shade preamble | **~2.4-2.7 M** | ~17 % |
| everything else in the raster | ~1.1 M | ~8 % |
| next-largest subsystem (CPU/JIT) | 2.24 M | 15.2 % |

The two fixed costs are **~5.5 M/frame — a quarter of every instruction we
execute, and ~37 % of the entire gap.** DraStic's counterpart for both jobs is
1,064.5 instructions per polygon, or 0.27 M/frame. They are the same defect
twice: work done per scanline or per span that they do once per polygon or once
per batch.

Sizings: the 2.88 M is an exact count (§3, `render_polygon_line` 841.5/line +
`span_stage` 239.4/line x 11.5 scanlines/polygon vs 1,064.5/polygon). The
shade term is the ~816-instructions-per-span intercept of the five-scene fit in
§2, which is directional (~15 % off on meteos and etody) — treat it as
2.4-2.7 M, not a precise figure. Not all of either is removable: the depth
pre-pass and the parts of `span_stage` that depend on live `depth_`/`attr_`
cannot hoist. **Credible landing zone: 3-4.5 M/frame, 14-20 % of our
instructions.**

### The rewrite, in three parts that ship together

**(a) Precompute the whole polygon's scanlines up front.** One per-polygon pass
producing arrays indexed by scanline — `xstart`, `xend`, both endpoints'
w/z/rgb/st, and the fill and coverage flags. Replaces the per-scanline
`setup_left_edge`/`setup_right_edge`, the fourteen `Interp::interpolate` calls
(~69/line), `edge_params` (~48/line) and the staging block in
`render_polygon_line` (~90/line). `Slope::step` is already a linear recurrence,
so this is a tight loop with the fill rules decided once. This is DraStic's
`setup_spans_asm_1x` + `interpolate_edges` + `setup_edge_markers`.

**(b) Make the staging kernels segmented.** *This is the part the old list
understated, and the one that decides whether (a) actually pays.*
`kern::active::span_factor` and `span_attrs5n` are invoked **once per scanline**
— roughly 314 and 456 instructions per call for a 13-35 pixel span. They cannot
simply move to once-per-batch: the perspective factor depends on each span's own
endpoints. They need a segmented form — an array of per-scanline descriptors
processed as one run with per-segment constants reloaded in the loop, or
per-pixel step values emitted during (a). Skip this and the per-span preamble
survives the restructure and only half the win lands.

**(c) Delete the per-part walk.** `walk_span` still makes three `draw_span`
calls per span even after `resolve_batch` hoisted the call overhead (both
`resolve_span<...>` specialisations are still 6.3 % of our instructions on
device). Write per-pixel edge/part/coverage flag bytes during staging so
`resolve_*` becomes one flat pass over `[0, batch_px_)` with no span structure
at all. The open problem — each span writing a different `row0` — is solved with
a per-pixel destination index written during staging; `batch_px_` is 256, so
that is 1 KB.

Mode specialisation, the fourth thing DraStic does (306 `render_*` variants), is
**already done** on our side via `select_resolve` / `select_gather4`. Not a
lever.

### What to watch while doing it

- **Byte-identical output is the bar.** `--dump-frames` on both builds, `cmp`.
  A speedup that changes output is measuring a different workload.
- **This trades instructions for memory traffic.** The precompute arrays are new
  writes and reads. §6's lesson is that duplication *underestimates*
  memory-bound costs, so price by removal and paired A/B on device, never by
  prediction. Keep the arrays small and consumed immediately — `RING=8` already
  showed we are sensitive to tile footprint. DraStic writes a per-scanline span
  array too, so the shape is proven.
- **It collides with the band split.** Each band worker must precompute only its
  own slice of a polygon's scanlines, or the precompute happens N times.

### Then

4. **The 2D dirty bit** — 14.4 % of the gap, fully designed and priced (§5), and
   the only other item with a measured ceiling. Independent of the raster work,
   so it is the right thing to pick up alongside it.
5. **SPU blocking** (3.5 %) — same shape as the renderer's fix, so cheaper after
   it than before.
6. **CPU/JIT** (15.2 %) has no identified single lever yet; the ARM7 combine and
   per-access cost accounting are the two candidates, at a measured 5.58 %
   ceiling for the latter.
