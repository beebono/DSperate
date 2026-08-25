# Plan: a complete binning and batching rasteriser

DraStic's 3D renderer costs 1.08 ms/frame on SM64DS at 32.6 % of cycles
([techniques/02](techniques/02-3d-software-rasteriser.md)). Ours is built on
the same three ideas and implements roughly half of each. This plan is about
closing the other half.

It exists because of one measurement. The parked `wip/span-batching` branch,
re-run against the tail metrics that
[`plan-cpu.md`](plan-cpu.md) added, moves **frames over the 16.715 ms budget
on `sm64` by -17.08 % (t = -22.36, 0/8 reps worse)** — 512.9 to 425.3 of 1800
— while the mean it was originally judged on is flat (-0.18 %, t = -1.35).
The same branch on `mlbis` regresses **every** metric, over-budget frames
**+6.99 % (t = 17.43, 7/7)**.

A change that both removes a sixth of one scene's dropped frames and adds a
fifteenth to another's is not a change to accept or reject. It is a change
whose gating condition we have not identified. Section 1 identifies it.

---

## 0. What we already have

Worth stating plainly, because three of DraStic's mechanisms are already
present in some form and the plan is about completing them, not introducing
them.

| Mechanism | DraStic | Ours | Gap |
|---|---|---|---|
| Screen split into cache-resident tiles | 12 bins × 16 lines | `render_chunk`, 14-line ring | geometry is close; **footprint is 3x** (§4) |
| Polygon-major within a tile | bin polygon list | `render_chunk` active-set merge | present |
| Parallel split | 12 bins / 4 threads | 3 work-balanced bands | present, different axis |
| Batch spans before the pixel stages | 256 px, span table | 256 px, `SpanJob[]` | **per-span cost model is inverted** (§1) |
| Stages over arrays, not pixels | ~15 NEON stages | `span_stage`/`span_texels`/`span_shade`/`resolve_span_vec` | present |
| Per-polygon specialisation at bin time | AND/OR constant detection, 133 kernels | none; span-level constant colour only | **absent** (§2) |
| Kernel bound once per polygon | direct calls, no indirect | `kResolve[mode][tex][aa][shadow]` per flush | **rebound per flush** (§3) |
| Fused per-bin resolve | 10 `resolve_bin_*`, keyed once | `final_pass(y)` per line | **absent** (§5) |

---

## 1. Why batching helps `sm64` and hurts `mlbis`

### The census predicts the opposite

[`profile-vs-drastic.md`](profile-vs-drastic.md) frames the renderer defect as
*frequency*: we call the pixel stages 7-11x more often than DraStic flushes.
By that model `mlbis` — the worst frequency ratio at 11.2x — should gain most
from batching. It is the only scene that has ever lost.

So frequency is not the gating variable. Batch **fill** is, and the scene
table in [`scenes/README.md`](../scenes/README.md) already has the column.

### Overhead is per span; benefit is per pixel

`flush_batch` amortises `span_texels` and `span_shade` over `batch_px_`
pixels. It pays for that with a `SpanJob` — thirteen fields, ~48 bytes —
**stored** per span in `render_polygon_line` and **reloaded** per span in the
flush loop. Unbatched, those values were computed and consumed in registers.

Benefit scales with pixels, cost with spans, so the tax per pixel is
`1 / mean_span`:

| scene | mean span | spans per pixel | mean | p90 | over-budget |
|---|---|---|---|---|---|
| `etody` | 35 px | **0.029** | **-0.53 %** | **-5.40 %** | -0.10 % |
| `sm64` | 32 px | 0.031 | -0.19 % | +0.10 % | **-17.09 %** |
| `dbori` | 17 px | 0.059 | +0.74 % | +1.54 % | +0.31 % |
| `mlbis` | **13 px** | **0.077** | **+1.79 %** | **+2.91 %** | **+6.88 %** |

8 paired reps per scene, 1800 frames, saves passed. Every mean and p90 figure
above is significant (|t| = 3.6 to 18.4) and unanimous or near-unanimous in
sign across reps.

**The mean-family ordering is monotone in `spans per pixel` across all four
scenes, with no exceptions**, and the crossover sits between 0.031 and 0.059.
That is the prediction of §1 and it is confirmed: `dbori` at 0.059 did land
nearer `mlbis` than `sm64`.

A batch is flushed per polygon, so a `mlbis` polygon at 142 px **can never
fill one** — it pays the full per-span bookkeeping and takes 55 % of the
amortisation. `etody` at 1073 px fills four and takes 84 %.

### The over-budget column does not follow, and that is a second effect

`sm64` is off the model. It gains **-17.09 % over-budget frames (t = -21.55,
0/8)** on a flat mean and a p99 that is *worse* by 2.55 %. Nothing in the
fill argument predicts that, and the fill argument does not get credit for it.

The mechanism is visible in where the budget line falls. `sm64` is over budget
on 28.5 % of frames, so 16.715 ms sits near its 71st percentile — in the thick
of the distribution, not the tail. Batching pushes the middle of the
distribution down across that line while adding variance at the extreme, which
is exactly a lower over-budget count with a worse p99.

`etody` shows the complement: a large, clean **p90 win (-5.40 %, t = -18.4,
0/8)** that buys almost no frames back, because its p90 is 23.5 ms and its
whole upper distribution stays far above budget regardless.

So there are two independent effects and they must be measured separately:
per-pixel amortisation (tracks `1/mean_span`, §1) and distribution shape
relative to the budget line (scene-specific, not modelled). **Do not fold the
branch in on the `sm64` over-budget number** — two of four scenes regress on
the mean family, and that number is not the thing §1 fixes.

### The previous save attempt gated on the wrong axis

`render_chunk` already carries one:

```cpp
if (!e.sh.textured) flush_batch(e.sh, mode);   // untextured: flush per span
```

The note in memory records that gating batching to textured polygons *did not*
help `mlbis`. That is consistent with §1: the gate removes the *benefit* for
untextured polygons but leaves the *cost*, because `render_polygon_line` still
writes a full `SpanJob` and `flush_batch` still runs its prologue, dispatch
selection and three-part `draw_span` loop for a single span. The correct axis
is span length or polygon pixel count, and it has never been tried.

### DraStic does not have this problem, structurally

Their batch is described by four words — span-table pointer, first scanline,
span count, pixel count — because the per-span data already lives in a **span
table built by a separate edge pass**
(`render_polygon_edge_interpolate_x_c`, once per polygon), which the
interpolation setup has to read anyway. Ours duplicates that state into a job
record at rasterisation time. Their span descriptor is free; ours costs 48
bytes of store-then-reload per span.

This is the same finding as our hottest function. `render_polygon_line` is
**#1 in the process on all four 3D-heavy scenes (11.3 %)** and it is
per-scanline *setup*, not pixel work. DraStic's architecture removes that work
from the per-scanline path entirely.

---

## 2. Constant attributes (the largest untaken win)

DraStic accumulates bitwise AND and OR of each vertex attribute while binning;
`AND == OR` means the attribute is constant over the polygon and selects a
kernel that skips its interpolation. `render_polygon_depth_compare_less_than_
constant_asm` alone is **2.28 % of SM64DS cycles — more than the interpolated
variant**.

We detect constant colour, but per span and only for colour
(`C_SPAN_FLAT_RGB`). The scene table says how much is on the table:

| scene | constant colour |
|---|---|
| `meteos` | **99 %** |
| `mlbis` | **67 %** |
| `sm64` | 54 % |
| `dbori` | 26 % |
| `etody` | 18 % |

Two thirds of `mlbis` and effectively all of `meteos` — and `mlbis` is exactly
the scene §1 says batching cannot help. Constant-W is the one that matters
most, because constant W makes a span **linear**, collapsing the two-stage
perspective interpolation in `Interp::set_x` to a single multiply-accumulate.
`render3d.cpp:588` already notes we never took the constant shortcut at all.

Detection is two instructions per vertex per attribute in `setup_polygon`,
which already walks the vertices.

---

## 3. Bind the kernel once per polygon

`flush_batch` rebuilds its dispatch on every call:

```cpp
ResolveFn resolve = kResolve[mode][sh.textured][(dispcnt >> 4) & 1][sh.shadow];
```

For an untextured polygon, `flush_batch` is called **once per span**, so this
is per-scanline dispatch — literally the "ours re-derives per scanline"
defect. `mode` is already hoisted to `render_chunk`; the function pointer
should join it, resolved once in `setup_shade` and stored in `Shade` beside
`gather4` (which is already done exactly this way — the precedent is in the
file).

Cheap, mechanical, and it is a precondition for §1's fast path: a
non-batching span path is only cheap if it has no dispatch in it.

---

## 4. The tile footprint is 3x DraStic's

Not previously noticed and cheap to test.

```cpp
static constexpr int W = 258, CHUNK = 14, RING = 16, RSIZE = W * RING;
std::array<u32, RSIZE * 2> color_{}, depth_{}, attr_{};
```

`RSIZE * 2 = 8256` words = **33 KB each**, three of them = **99 KB**, plus 4 KB
stencil. DraStic's whole live tile is 32-36 KB, deliberately sized to the
A55's 32 KB L1D. Even counting only the top pixel layer (the `* 2` half is the
AA under-pixel, touched on edges) we are at ~50 KB.

Two independent levers, both nearly free to try:

- **`CHUNK = 6`, `RING = 8`** halves it: 24.75 KB hot, 49.5 KB total. One-line
  change; `RING` must stay a power of two ≥ `CHUNK + 2` for `row_of`'s mask.
- **Pack `depth_` and `attr_`** into one buffer as DraStic does, removing a
  third of the footprint and one stream from every span.

The first is a sweep, not a redesign, and it has never been run. Do it before
anything in §1 — if 3D scenes move on a chunk-size sweep alone, the residency
argument is real and §5's fused resolve gets more valuable.

---

## 5. Fuse the resolve into the tile

`final_pass(y)` runs per line, reading the two neighbouring ring lines for
edge marking, then fog, then AA blend, then the output write. DraStic keys one
of ten `resolve_bin_*` kernels **once per bin** from a 3-bit
`(edge_marking << 2) | fog_mode`, and does every enabled effect in registers
between one read and one write.

The scene table says this is currently small — `3d final pass` is 0.5-0.9 % on
four scenes — with one exception: **`dbori` at 3.5 %**, four to seven times
every other scene. `dbori` is also the scene with the worst depth survival
(61 %) and 54.1 % of frames over budget, the highest of the five. Worth
sizing on `dbori` specifically rather than dismissing on the mean.

Lowest priority of the five, and listed so it is not lost.

---

## Results so far

### s4, footprint: confirmed, and the cheapest win here

`RING` 16 -> 8 halves the live tile from 99 KB to 50 KB (24.75 KB of it the
hot top layer, which now fits the A55's 32 KB L1D). Paired, 1800 frames, 6
reps, output byte-identical:

| scene | mean | p90 | over-budget |
|---|---|---|---|
| `mlbis` | **-1.64 %** (t = -18.3, 0/6) | -0.99 % | **-9.03 %** (t = -15.8, 0/6) |
| `sm64` | **-0.91 %** (t = -17.6, 0/6) | -0.89 % | -1.01 % |
| `etody` | -0.69 % (t = -2.6) | +0.69 % | +0.17 % |
| `meteos` | -0.63 % (t = -4.1, 0/6) | -0.72 % | -2.20 % |
| `dbori` | -0.30 % (t = -3.9, 0/6) | **-1.33 %** (t = -29.3, 0/6) | -0.20 % |

Every scene improves on the mean, five for five, and it is orthogonal to
anything about spans -- a smaller tile helps whichever path runs. The
residency argument in s4 is real, so s5's fused per-bin resolve and s4's
depth/attr packing both gain value.

### The first gate attempt regressed, and the cause was not the gate

Factoring the per-span edge/fill walk into `resolve_one` so the batched and
direct paths could share it turned it into a **real call with a 112-byte
frame and five register-pair saves, taken once per span on both paths**.
Against `st-base`, 6 paired reps:

| scene | gate vs st-base | ungated batch was | implied cycles/span |
|---|---|---|---|
| `etody` | +1.47 % | -0.53 % | 93 |
| `sm64` | +1.51 % | -0.19 % | 117 |
| `dbori` | +0.80 % | +0.74 % | 94 |
| `mlbis` | +3.07 % | +1.79 % | 176 |

A near-constant per-span tax across four scenes with very different polygon
mixes is the signature of a per-span call, and the disassembly showed it
directly. `always_inline` removes the symbol and both call sites.

**So that run says nothing about s1's premise** -- the cost introduced was
larger than the cost under test. Stage 3's kill criterion (mlbis not rescued)
technically fired and is explicitly **not** being called on this evidence.

Two lessons worth carrying, both already paid for once here:

- Sharing code between a hot path and a cold one is not free. The saving s1
  is chasing is tens of cycles a span; a call frame is roughly a hundred.
  Check the disassembly for the symbol before believing any measurement of a
  refactor like this.
- `objdump` on the host silently produces an **empty file** for an aarch64
  binary rather than failing. Use `aarch64-linux-gnu-objdump`, and check the
  output size before reading a conclusion out of it.

---

## Staging

Each stage is independently measurable and independently revertable. Kill
criteria are stated so a stage that does not pay gets dropped rather than
carried.

| # | Work | Cost | Status / kill if |
|---|---|---|---|
| ~~0~~ | ~~Check s1's ordering on `dbori`/`etody`~~ | done | **confirmed monotone, 4/4** |
| ~~1~~ | ~~s4 chunk sweep, `RING` 16 vs 8~~ | done | **-0.30 to -1.64 % mean, 5/5 scenes** |
| ~~2~~ | ~~s3 bind resolve fn in `setup_shade`~~ | done | landed, output-neutral |
| **3** | s1 fast path: skip `SpanJob` below a mean-span threshold (24 px; sweep 16/32) | medium | **re-running after the inline fix** -- mlbis regression not removed => the tax is not `SpanJob`, re-diagnose |
| 4 | s1 proper: span table from a separate edge pass, batch = 4 words | **large** | only start if stage 3 confirms the mechanism |
| 5 | s2 constant W, then Z, then U/V | medium each | per-attribute; drop any that does not pay |
| 6 | s4 pack `depth_`+`attr_` into one buffer | medium | promoted: s4 is confirmed |
| 7 | s5 fused resolve, sized on `dbori` | medium | promoted: s4 is confirmed; `dbori` does not move => drop |

Two cheap items are also now known to be worth doing regardless of stage 3:
the shoelace in `batch_worth_it` runs once per polygon **per band** (three
times a frame) because every band repeats `build_edges`, and `Shade` grew by
a 16-byte pointer-to-member inside a 2048-entry array. Neither is large
enough to explain the regression above, and both should be cleaned up before
stage 3's result is read as final.

## Measurement protocol

Unchanged from [`techniques/00`](techniques/00-method-and-measurements.md) and
non-negotiable, because every wrong call in this project came from skipping a
piece of it:

- **All 1800 frames, with the save.** The `3d raster 0.8x` figure that
  currently underwrites "our renderer already matches DraStic" was taken on
  **300 frames of direct boot** — and every 3D-heavy cluster we located starts
  after frame 450. `etody` draws 25 K polygon lines in its first 450 frames
  against 4.77 M in its bad window. That number is not wrong, it is measured
  on the wrong third of the program.
- **Paired interleaved reps**, `mean(delta_i)` with standard error and
  faster-in-N-of-M. Never two separately averaged runs.
- **Report over-budget frames, p99 and max, not only the mean.** This whole
  plan exists because `sm64` moved 17 % on a metric that was not being
  reported when the branch was parked on "+0.04 %, neutral".
- **Byte-identical frames** across the A/B for anything that is not
  deliberately changing output. Every stage here is output-neutral.
- Fold in on the majority of scenes, and record per-scene numbers — §1 is the
  case for expecting a change to help some scenes and hurt others.

---

## Out of scope

Unchanged from [`profile-vs-drastic.md`](profile-vs-drastic.md): the pixel
stages, a depth pre-pass, the texture cache and the scheduler stay closed.

One correction to that document's framing. Its "we match or beat DraStic"
rulings rest on share-of-frame ratios, which are scale-free — they say where
*our* time goes, not how our absolute cost compares. The single absolute
comparison is the 300-frame boot figure above. The honest statement is
**"unmeasured in absolute terms on the workloads that miss frames"**, not
"we win". Nothing here is positive evidence against those four; they stay
below the five items in this plan, but "already beaten" should not be the
reason.
