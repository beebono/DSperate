# Implementation checklist

A compressed index of every high-level technique in documents 01–05, one line
each, for auditing DSperate against DraStic. Each row names the technique,
the DraStic document that describes it, and a column for our status.

Audited 2026-08-28 against `src/` at 5e99f73 by reading the code, not by measurement; `partial`/`equiv` rows are judgement calls and say why. Rows 4.11–4.16 re-audited after lazy 2D landed (same day). The **DSperate** column uses: `same` (we do this), `equiv` (we
achieve the same effect a different way — say how in a note), `partial`,
`no`, or `n/a` (does not apply to our design — say why). The point is not to
copy DraStic row by row; it is to make sure every deliberate *omission* is a
decision rather than an oversight. Rows that our own measurements have
already settled carry a note.

## Audit summary (2026-08-28)

105 rows: 43 `same`, 24 `equiv`, 15 `partial`, 14 `no`, 5 `n/a`, none
still marked high priority (5.1 and 4.8 moved `no` → `partial` on 2026-08-28). Where DSperate departs it is usually because it chose
hardware-exact, melonDS-comparable behaviour (DMA per unit, SPU per sample,
geometry per slice) over DraStic's deferral — those are the rows worth a
measured decision, ranked by likely payoff:

1. ~~**Lazy 2D with a write journal** (4.12–4.15)~~ — done: one batch at
   the last display line, one worker hand-off, per-engine journal; frames
   byte-identical on all five scenes in every mode (see the 04 §5 rows).
2. ~~**SPU mixed per sample as a scheduler event** (5.1)~~ — done, the
   exact half: one event per 16 samples (`DS_SPU_BATCH`), channels still
   stepped per sample, register reads/writes catch the mixer up first, so
   `DS_SPU_BATCH=1` reproduces the old output byte for byte. Two things it
   surfaced: (a) the per-sample event had been the *de-facto interleave
   cap* — the SDK's IPCSYNC boot countdown never completes when one CPU
   runs > ~2.5 k cycles unanswered (SM64DS: 2048 boots, 2560 does not), so
   event-bound mode now caps slices at `EVENT_BOUND_QUANTUM` = 2048
   explicitly; (b) with the cap doing that job, both-halted slices run to
   the real deadline: meteos 654 k → 402 k slices per 600 frames. Host
   callgrind, 120 frames: SM64DS −0.5 %, meteos −0.8 % instructions
   (`fire_due` halved). Guest
   timing shifts by a constant phase (meteos ARM7 driver +14 lines; SM64DS,
   etody, dbori frame hashes move as in the timer-race notes; mlbis and
   meteos hashes unchanged), so audio is not byte-comparable to the old
   build except through `DS_SPU_BATCH=1`. The inexact rows (5.4, 5.6, 5.9,
   5.11) stay `no`: they would change what Rhythm Heaven's just-in-time
   stream writer sees, and the remaining per-sample channel work is small.
3. ~~**Geometry: per-word enqueue, per-polygon constants, batched vertex
   transform** (4.17, 4.18)~~ — done in the exact form; *not* frame-granular replay. The per-slice
   `run_to` is an inline idle test and `run_to_slow` runs only 148–567×
   a frame, so dispatch count is not the cost; and the games drive the
   engine at FIFO pace (`check_fifo_irq` 23 k/frame on GSDD, FIFO-full
   stalls live), which replay would have to fake. qemu census on 885b774,
   per frame: GSDD title 13.5 M geometry of 51 M (20 %; largest share of
   any scene), dbori 4.7 M of 25.6 M. Where it sits: `gxfifo_write`
   65 insn/word × 52 k words = 3.4 M (GSDD) / 1.4 M (dbori);
   `submit_polygon` 455–537 insn × 5.3 k = 2.8 M / 0.6 M; `run_to_slow`
   self (execute loop, per-command transform/lighting) 3.3 M / 1.6 M;
   `submit_vertex` ~105 insn × 14 k = 1.5 M / 0.4 M; `exec_single`
   dispatch 34–44 insn × 41 k = 1.4 M / 0.4 M. Targets in that order:
   the enqueue path, the polygon submit constants, DraStic's batched
   vertex transform (4.18) — keeping execution slice-granular.
   *Enqueue path done:* pipe, FIFO and stall queue are one ring with three
   counts (no entry is copied between stages), `gxfifo_write` takes a
   mid-command parameter without the packed-command walk, the DMA unit
   timing is cached per 16 KB block and a GXFIFO DMA feeds a direct-mapped
   source page as a run. Frame hashes identical on all five scenes and
   1800 GSDD frames. qemu census, non-spin: GSDD 51.2 → 49.7 M (−2.9 %),
   dbori 25.6 → 24.9 M (−2.8 %); `Dma::run_channel` −0.5 M / −0.3 M,
   geometry −1.0 M / −0.4 M. `gxfifo_write` itself is still ~65 insn a
   word on GSDD (its words are one-parameter commands, so the walk runs
   for most of them). *Submit constants, first cut:* `clip_polygon` tests
   all six planes once and skips the three copying passes when nothing
   clips (the colour truncation is idempotent, applied once) —
   `submit_polygon` 535 → 493 insn/polygon on GSDD, 453 → 386 on dbori;
   exact. Cumulative from 885b774, non-spin: GSDD 51.2 → 47.1 M
   (−8.1 %), dbori 25.6 → 24.1 M (−5.7 %). *Execute loop:* the
   per-command timing helpers, `execute` and `exec_single` pinned inline
   (LTO had left them as calls), the FIFO IRQ check gated on the mode and
   the GXFIFO-DMA check on a kept armed flag: a further −0.8 % / −1.0 %.
   Tried and rejected, measured: a NEON form of the vertex transform
   (110 → 118 insn/vertex; the scalar `smull`/`smaddl` chain is already
   the better AArch64 code). **Item closed in its exact form**: what is
   left is per-command work at ~65 insn/command in the execute loop,
   ~110/vertex and ~495/polygon, all of it arithmetic the hardware also
   does; DraStic's remaining gap (4.17/4.18) is the log-replay model,
   which this project has decided against. Final, excluding both thread
   spin-waits: GSDD 46.0 M, dbori 23.3 M non-spin insn/frame.
4. ~~**DMA per unit through the bus** (4.8, 4.9)~~ — done in the exact
   form: a word transfer between two direct-mapped pages (and a GXFIFO
   feed from one) runs to the page end on host pointers — one page-table
   walk per end per run, not two per word — and the per-16 KB unit timing
   is cached; every word is still charged, stall-checked and budget-bounded
   as before, so the unit model (4.9) is unchanged and the copy is not
   front-loaded. `Dma::run_channel` GSDD 7.0 → 3.1 M/frame, dbori 1.8 →
   0.9 M; frame hashes identical everywhere. What is left is the per-word
   unit-timing model itself (~30 insn/word) — DraStic's 7.7 k/frame is
   what a whole-transfer copy with a static cost table buys, at the price
   of the unit model; not worth it at 6 % of GSDD's frame. *Second cut,
   2026-08-28:* inside a direct-mapped run the region pair and n/s costs are
   fixed (a 2 KB page never crosses a 16 KB timing block), so the per-word
   `unit_cycles` call is replaced by one constant or the same burst-table walk
   (`Dma::run_cost`); and `gxfifo_write` finishes a one-parameter command
   without the packed-command walk (40 k words a frame on GSDD). Exact on all
   six hash sets; qemu non-spin GSDD 46.2 → 42.7 M (−7.7 %), dbori 23.3 →
   22.4 M (−3.9 %), `gxfifo_write` 3.57 → 1.97 M. **RG DS, paired, 4 reps:
   GSDD 27.0 → 25.6 ms mean (−5.4 %), median 31.7 → 29.8; dbori −1.6 %.** *Third cut (geometry, same day):* the clip
   passes take the working vertex by reference (60-byte copies per vertex
   per pass; GSDD's screen-sized quads clip every frame), the two GXSTAT
   busy-bit clears in the execute loop run only when a bit is set,
   `fifo_write` is pinned inline, and `mtx_mult_4x4` (rebuilds the clip
   matrix 3.5 k times a frame on GSDD, 43 % of `submit_vertex`) has an
   integer-identical NEON form. Exact on all six sets; qemu non-spin GSDD
   −1.1 %, dbori −0.7 %; RG DS paired, 4 reps: GSDD 25.6 → 25.0 ms mean
   (−2.6 %), median 29.9 → 29.0; dbori flat. Per-line attribution
   (`hbline.py`: hotblocks + `objdump --dwarf=decodedline`) found these.
5. **JIT: ITCM tag-free tables, three arenas, known-constant tracking,
   check-free second entry** (1.5, 1.12, 1.23, 1.26) — each small; the
   arena flush (1.23) is the one with a visible failure mode (full arena
   throws away every translation).
6. ~~**3D: 8-way gather, AND/OR uniformity, constant-W ramp** (2.4, 2.11,
   2.14)~~ — settled 2026-08-31. Constant-W and flat-rgb uniformity measured
   empty (dbori 0.02 % constant-W pixels; GSDD's already take `_lin`); the
   axis with signal was constant *alpha*: `Shade::opaque` proves a polygon
   all-opaque at setup (alpha 31, decal or a 0/31-alpha texture format,
   riding span_shade's alpha-narrowed pass plane) and selects an opaque
   resolve instantiation with the translucent machinery compiled out.
   With the 3.15 capture kernels, device (paired, 3 reps after warm-up,
   1800 frames): etody −3.8 % mean / −7.4 % p99 (over-budget 16 → 10),
   mlbis −2.0 % / −2.8 %, dbori p99 −2.1 %, GSDD/sm64/meteos flat, no
   regressions. 2.14 (8-way gather) remains the one unmeasured kernel row.
7. **Texture alpha map** (branch `alpha-map`, parked opt-in `DS_AMAP=1`) —
   39 % of GSDD's depth-passing pixels sample alpha-0 texels; skipping and
   trimming those spans is exact (all six hash sets) but a device loss
   (`3d spans` +4 % GSDD, +8 % dbori): the per-span tests cost more than the
   ~25-instruction resolve they save, and the vector resolve pays for masked
   lanes anyway. Keep only if the raster becomes the critical path.
8. ~~**Threads and per-line frames**~~ — done 2026-08-29: three raster workers
   pinned (knob sweep: mlbis −6.8 %, etody −2.6 %, sm64 −1.4 %; the 2↔3
   controller is `DS_R3D_ADAPT=1`), `DS_R3D_SKIPDUP` default, and capture
   frames batched (the lazy-2D addendum in 04 §5 below). Measured and parked:
   a lagged engine-B hand-off (`DS_2D_LAG=1`) — exact, but the trap it needs
   loses to Golden Sun's ~3.5 k VRAM stores a frame. Not adopted: RING 16,
   bins other than 8, SPU batch 64, a main-thread-demoting controller,
   per-subsystem threads beyond four (four cores; five threads already
   oversubscribe it on GSDD).

## 01 — Recompiler

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 1.1 | Guest r0–r14 pinned to host registers for the whole run | 01 §1 | same | r0–r7, r13, r14 in x19–x28; r8–r12 in x9–x13 (`jit_internal.h`) |
| 1.2 | Guest r6–r14 in the AAPCS64 callee-saved range so helpers spill 6 not 15 | 01 §1 | same | ten of fifteen callee-saved; call stubs spill only r8–r12 |
| 1.3 | Guest CPSR flags live in host NZCV | 01 §1 | same | plus a flag-liveness pass DraStic does not have (bare `tst` when C/V dead) |
| 1.4 | One register doubles as page-table base and helper spill base | 01 §1 | no | page-table base x14 and timing table x15 are separate pinned registers; spills go via x29 (`CpuContext*`) |
| 1.5 | Two-instruction block prologue (`TBZ` on cycle sign bit); second entry point skips it | 01 §2 | partial | one `tbnz w8,#31` per block; no second check-free entry point for post-event re-entry |
| 1.6 | Cycle cost accumulated per block, one `SUB` emitted | 01 §2 | same | static costs batched into one `sub` per run (`flush_pending`) |
| 1.7 | Downward cycle counter tested by sign, not compared | 01 §2 | same | `budget - 1` in w8, sign-bit test |
| 1.8 | Per-CPU cycle model (ARM7 ×2, ARM9 + adjustment) with per-game hacks | 01 §2 | equiv | interpreter model reproduced exactly (per-page timing table, CD/CDI formulas); no per-game hacks |
| 1.9 | Direct linking of static branches, delayed patch list for untranslated targets | 01 §3a | same | `bl link; .word key` patched to a bare `b` (`jit_h_link`) |
| 1.10 | Link may target the check-free entry; spin-loops forced out via `mov w12,#-1` | 01 §3a | equiv | no forced-negative budget; spin loops handled by idle-loop detection (`idle_loop.cpp`, DS_IDLE_SKIP) |
| 1.11 | 1024-entry inline direct-mapped branch-target cache in the CPU struct | 01 §3b | equiv | per-CPU direct-mapped LUT at the front of the arena, 7–8 insns; sized per "The branch LUT" in jit/README |
| 1.12 | Tag-free direct tables for ITCM (ARM 8192 / Thumb 16384 slots) | 01 §3c | no | ITCM goes through the same LUT (tagged); no tag-free table |
| 1.13 | 32-bit cache-relative offsets for all block/metadata pointers | 01 §3b | same | LUT entry = `(native offset << 32) | key` |
| 1.14 | Flat 2 KB-page table per CPU (16 MB), pre-biased base, entry stored `>>2` | 01 §4 | same | `PAGE_SHIFT = 11`, tagged entries, pre-biased base |
| 1.15 | Zero entry covers unmapped, I/O and side-effect pages in one `cbz` | 01 §4 | same | `cbz` on the entry |
| 1.16 | Store path flag bits: 62 = slow path, 63 = code page | 01 §4 | same | two tag bits: MMIO/RO and `TAG_CODE` |
| 1.17 | Loads never test the flag bits | 01 §4 | same |  |
| 1.18 | SMC filter 1: code-page bit test | 01 §5 | same | store path tests the tag |
| 1.19 | SMC filter 2: silent-store elimination (compare before invalidate) | 01 §5 | same | `mem::store_code` drops identical values — done — see JIT SMC filters note |
| 1.20 | SMC filter 3: block-range allocation check | 01 §5 | same | `invalidate_host_range` kills only overlapping blocks — done — see JIT SMC filters note |
| 1.21 | Self-invalidating block recovers PC from metadata, flushes, re-enters | 01 §5 | equiv | killed block gets a dispatcher redirect + alert word; running block leaves at its next poll rather than re-looking-up in place |
| 1.22 | Compressed host-offset → guest-PC map per block | 01 §5 | partial | no per-block host→guest PC map; fallback/poll sites carry the key as a literal instead |
| 1.23 | Three code arenas (ITCM / main RAM / other) with separate flushes | 01 §6 | no | single arena, `reset_arena()` flushes everything when full |
| 1.24 | Code grows forward, metadata backward in one arena | 01 §6 | no | metadata in a side `Block` structure |
| 1.25 | Direct opcode emission, no IR | 01 §7 | same | `emit.h` direct encoder, no IR |
| 1.26 | Per-register known-constant tracking with `MOVZ/MOVK/ORR`-imm materialisation | 01 §7 | partial | constants only for pc-relative operands; no per-register known-value tracking |
| 1.27 | Intra-block forward-branch fix-up pass | 01 §7 | same | hot/cold split with fixups at the splice |
| 1.28 | Sixteen specialised LDM/STM helpers per direction | 01 §7 | equiv | LDM/STM inlined when the transfer sits in one 2 KB page, interpreter fallback otherwise; no per-count helpers |
| 1.29 | GXFIFO / geometry ports recognised inline in the ARM9 32-bit store helper | 01 §8 | same | done — DMA→GXFIFO dispatch bypassed; see dbori note |

## 02 — 3D rasteriser

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 2.1 | 12 bins × 16 scanlines; ~32 KB live tile footprint | 02 §1 | equiv | ring of RING=8 lines (~50 KB) per worker, chunked rasterisation; bins are horizontal bands — measured: pays with 2 workers / 8 even bins — see binning note |
| 2.2 | Branch-free 12-bit bin mask from ymin/ymax | 02 §1 | equiv | difference array + prefix sum over polygon line ranges (`compute_bins`) |
| 2.3 | Bins ÷ thread count for even split on 1/2/3/4/6/12 cores | 02 §1 | equiv | 8 even bins claimed dynamically by an adaptive worker count — adaptive worker controller instead — see note |
| 2.4 | AND/OR uniformity test per polygon selects `_constant` kernels | 02 §2 | equiv | `attrs_constant` / `rgb_constant` on the Shade, per-span endpoint equality (`span_attrs2n`), and `Shade::opaque` (2026-08-31): a polygon provably all-opaque (alpha 31, no A3I5/A5I3) takes an opaque resolve instantiation with the translucent machinery compiled out — decided at setup from the polygon, no per-pixel AND/OR pass needed (constant-W/flat-rgb uniformity measured empty; constant-alpha was the axis with signal). Device: etody −3.8 % mean / −7.4 % p99 (with the capture kernels), mlbis −2 %, dbori p99 −2 % |
| 2.5 | Specialisation matrix: 9 wrap × 8 combine × 10 resolve × 4 depth | 02 §2 | equiv | resolve is a `[mode][textured][aa][opaque]` vector matrix (+`[shadow]` scalar); wrap modes templated in the gather (`gatherN_wraps`) |
| 2.6 | Fused resolve (edge mark + fog + convert + store) chosen once per bin | 02 §2 | equiv | `final_pass` per line does edge mark + fog + AA in one pass |
| 2.7 | Spans batched to 256 px across scanlines before the pipeline runs | 02 §3 | no | measured flat — see per-span-cost note |
| 2.8 | Per-256-px kernel selection; flush has 39 direct calls, no indirect | 02 §3 | same | `ResolveFn` picked once per polygon in `setup_shade`; batch loops jobs internally |
| 2.9 | Stage-at-a-time SoA span pipeline; conditionals become byte masks | 02 §3 | same | span_factor → span_attrs5n → texels → shade → depth_candidates, all NEON passes over SpanBuf arrays |
| 2.10 | Per-span interpolants broadcast to per-pixel arrays, then one flat pass | 02 §3 | no | measured: does not port (+0.6–1.7 %) — see 02 §3 |
| 2.11 | Constant-W perspective factor as iota ramp, no division | 02 §3 | partial | `Interp::recip` turns the linear division into a multiply; no iota-ramp kernel for constant W |
| 2.12 | Depth test emits mask + surviving count; later stages skip on zero | 02 §3 | equiv | `depth_candidates` returns a pass mask and first/last range, later stages clip to it |
| 2.13 | Mask-shift tail handling, no scalar epilogue | 02 §3 | partial | arrays padded to multiples of 4/8 instead |
| 2.14 | Eight-way unrolled scalar texel gather for in-order latency hiding | 02 §4 | partial | `gather4_impl` issues 4 independent scalar chains per vector (DraStic: 8) |
| 2.15 | Software-pipelined vector loops (load next before store current) | 02 §4 | partial | not systematically; compiler-scheduled intrinsics |
| 2.16 | Fixed point throughout, no FP | 02 §4 | same |  |
| 2.17 | Texture lookup hoisted to bin time, one-entry memo | 02 §5 | same | `texcache_.lookup` once per polygon in `setup_shade` (hash map, content-validated) |
| 2.18 | Opaque then translucent pass with per-tile polygon-ID stencil | 02 §6 | equiv | per-line shadow stencil row in the ring; opaque/translucent ordering by list |
| 2.19 | Rear-plane bitmap path with scroll | 02 §6 | same | `clear_line` handles the rear-plane bitmap |
| 2.20 | Paired `_c` / `_asm` kernels, C as bit-exact reference | 02 §7 | same | `kernels_ref.cpp` / `kernels_neon.cpp` diffed by `tests/kernels_test.cpp` |
| 2.21 | `_1x` / `_4x` (hi-res) kernel families | 02 §2 | n/a | no hi-res mode |

## 03 — 2D engines

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 3.1 | 1-bit-per-pixel visibility masks; scanline = two NEON registers | 03 §2 | no | layer lines are u16 values with bit 15 = opaque; select kernels test the bit per lane |
| 3.2 | NEON movemask idiom (`and` + `addp` tree) for 4/8/12/16 bpp | 03 §2 | n/a | no bitmask representation |
| 3.3 | Bit-parallel priority encoder yielding first *and* second layer | 03 §3 | equiv | `select16` writes top and second value + table id per pixel in one pass (16-bit lanes, not 1-bit) |
| 3.4 | Separate OBJ first/second accumulators for forced-blend sprites | 03 §3 | equiv | `select16_obj` carries the attribute byte (semi/bitmap) into the resolve |
| 3.5 | `_single` encoder when blending is off | 03 §3 | same | `select16_flat` / `_flat_nowin` when `needs_second()` is false |
| 3.6 | Backdrop mask from accumulated coverage | 03 §3 | equiv | T_BACKDROP table id where nothing won |
| 3.7 | Pixel select 32 px/iteration; zero mask word skips the group | 03 §4 | partial | `Layer::any` skips an empty *line*; no per-32-px group skip |
| 3.8 | Planar 6-bit channel split once per line; effects as byte ops | 03 §5 | equiv | 18-bit packed records (6 bits/channel) resolved once on the winner; effects on packed words, not planar bytes |
| 3.9 | `tbl` + `ld2`/`st2` as a 4bpp palette unit | 03 §6 | equiv | `text_row_16` uses `vqtbl4q` for 16-colour tiles (64-byte table) |
| 3.10 | `bit` + `rev32`/`sli` branch-free H-flip, four tiles at a time | 03 §6 | same | `vrev64` + `vbsl` on the tile control flag (`kernels_neon.cpp:321`) |
| 3.11 | Windows as mask ANDs, one kernel per active-window count | 03 §7 | equiv | window plane byte per pixel; `_nowin` kernel variants for the common case |
| 3.12 | Mosaic applied to pixels and mask before priority | 03 §7 | same | BG/OBJ mosaic applied on the layer lines before select |
| 3.13 | Blank-layer elimination before the encoder | 03 §7 | same | `Layer::any` + `select_layers` |
| 3.14 | 3D output presented as an ordinary layer (visibility + alpha gather) | 03 §7 | same | `layer16_3d`, `line_has_translucent_3d` |
| 3.15 | Fused display-capture variants | 03 §7 | same | `capture_a15` / `capture_blend` ref+NEON kernel pairs (byte planes off one vld4, blend as multiply-long in halfword lanes); B-only mode is a memcpy; mode picked once per line (2026-08-31) |
| 3.16 | Assembly conversion only where profiling justified it (`obj_c` stays C) | 03 §8 | same | sprite rows have NEON kernels (`obj_row_*`); affine/large BGs stay C++ |

## 04 — Scheduler, deferral, DMA, memory

> **Lazy 2D (rows 4.11–4.16) — done.** `Gpu` (`src/core/gpu/gpu.cpp`) journals every 2D register, palette, OAM, POWCNT and MASTER_BRIGHT write per engine with the display line it first affects, and renders the frame in one batch at the last display line's HBlank — engine B on the line worker, engine A on the emulation thread, one hand-off — replaying the journal in front of each line. VRAM, which the journal cannot cover, is handled by a page-table write trap on the pages the engines read: the first trapped store of a frame renders every line whose HBlank has passed *before* the bytes change, then the frame continues per line. Capture and FIFO frames run per line from the start. Verified byte-identical against the per-line renderer on all five recorded scenes (1800 frames each; lazy, `DS_2D_LAZY=0`, `DS_2D_THREAD=0`, interpreter and JIT). Census (`DS_PROFILE=1`): mlbis/meteos/sm64 batch ~1800 of 1800 frames with 13–17 trap hits per run; etody 970, dbori 814 (the rest are capture frames). Choice recorded here: DraStic ignores CPU stores into VRAM mid-frame; we detect them and fall back, so the output stays hardware-exact. Measured on the RG DS (`dsperate-headless`, 900 frames, 3 paired reps each, warm-up run discarded), mean frame ms base → lazy: meteos 6.07 → 5.34 (−12 %), sm64 7.88 → 7.01 (−11 %), mlbis 1.93 → 1.52 (−21 %), dbori 7.29 → 6.73 (−7 %), etody 10.33 → 10.24 (flat: half its frames capture, per-line by design). Medians move the same way.
>
> **2026-08-29 addendum — capture frames batch too.** `lazy_frame_` no longer excludes capture frames (`DS_2D_LAZY_CAPTURE=0` restores per line): capture runs inside the batch in line order, its LCDC banks are trapped, and the captured bytes land at the last display line instead of per line — the one accepted inexactness, visible only to a CPU read of the capture bank between a line and line 191 (never in the six recorded sets: still hash-identical). The picture, the register timing (POWCNT1 swap, DISPCNT, VRAMCNT — journal replay and remap catch-up) and every VRAM write stay exact, which is what rules out the deferred-capture failure modes (screen flicker, a stuck virtual swap). A trapped store now catches up, renders eight lines per line without the trap, then re-arms and re-batches; word and halfword DMA runs take the trap once per run; and a catch-up of fewer than 24 lines against a parked line worker is drawn on the emulation thread — the worker's wake-up was the whole cost of batching on Golden Sun (~14 catch-ups a frame). Also fixed on the way: `Bus::update_vram` rebuilt the VRAM views before the lazy catch-up, so a mid-frame remap with pending lines rendered them against the new mapping (latent until a lazy frame could survive a trap hit). Device (RG DS, paired vs pinned-3 main): etody −5.4 % mean / −12.4 % median (SDL frontend: over-budget frames ~60 → ~20 of 1800), GSDD −3.7 % / −2.3 %, sm64 −1.2 %, dbori median −3.7 %, meteos and mlbis flat. GSDD lazy frames 525 → 1801 of 1800.

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 4.1 | Delta-encoded fixed-slot event list; slice = head delta | 04 §1 | equiv | fixed `EventId` slots, absolute `at_[]` deadlines, armed bitmask, cached `next_` |
| 4.2 | Both CPUs run the same slice; ARM7 at doubled cycle cost | 04 §1 | same | event-bound mode (quantum 0): ARM9 slice then ARM7 with `arm7_debt_/2`, slices capped at `EVENT_BOUND_QUANTUM` = 2048 (the SDK's IPCSYNC boot countdown needs it; the cap was implicit in the per-sample SPU event until 2026-08-28); verification harness uses 128-cycle lockstep |
| 4.3 | Forced task switch at next 128-cycle boundary on cross-CPU dependency | 04 §1 | equiv | `preempt()` on DMA start, `gx_fifo_full()` stall, LOCKSTEP_QUANTUM polling while stalled |
| 4.4 | Scanline as two events (3,072 + 1,188 cycles) | 04 §1 | same | HBlank / VBlank_Scanline events |
| 4.5 | Timer count derived on read; overflow as an event | 04 §2 | same | `timer_value` from `start_time`; overflow scheduled from the sample point |
| 4.6 | `irq_pending` precomputed at every IF-setting site | 04 §3 | same | `hot.irq_pending` |
| 4.7 | `pending_actions` + alert thunks at block boundaries only | 04 §3 | same | `hot.alerts` polled after stores / at fallback |
| 4.8 | Whole-transfer DMA over a 16-entry 8 MB region table | 04 §4 | partial | `Dma::run` executes per unit against a budget with burst timing (melonDS model); between two direct-mapped pages (or into GXFIFO from one) a run of words goes through host pointers, one page-table walk per end per run; I/O, trapped and code pages stay per word through the bus |
| 4.9 | DMA cycle cost from static seq/non-seq tables; completion as event | 04 §4 | no | per-unit burst tables (the per-16 KB block costs cached on the channel); the copy is not front-loaded |
| 4.10 | Coarse (64 KB) / fine (2 KB) code bitmaps ORed over DMA destination | 04 §4 | equiv | DMA words take the tagged-page store path → `store_code`; no separate bitmap |
| 4.11 | HBlank DMA into VRAM forces 2D render catch-up first | 04 §4 | equiv | any store into VRAM the engines read (DMA or CPU) hits the page-table write trap and catches the render up first (`Gpu::vram_store_trap`); DraStic only catches HBlank DMA |
| 4.12 | 2D rendered as one batch at VBlank when nothing changed mid-frame | 04 §5 | same | one `render_lines(0, 191)` at the HBlank of line 191; writes mid-frame do not break the batch (journal); a VRAM store catches up, runs eight lines per line, re-batches (trap); capture frames batch since 2026-08-29 (bytes land at line 191); FIFO frames per line |
| 4.13 | Engine B rendered on a worker thread, engine A on main | 04 §5 | same | one `LineWorker` dispatch per batch (per line only in fallback frames); each engine's output stage runs on its own thread |
| 4.14 | Per-engine journal of mid-frame register/palette/OAM writes, replayed per line | 04 §5 | same | `Engine2D::queue` / `replay_to`, stamped `line*2 + phase` so writes before and after a line's scanline start (window edges) replay in order; POWCNT and MASTER_BRIGHT ride the same journal |
| 4.15 | Copy-on-first-write shadow palette/OAM; journal only if value changed | 04 §5 | equiv | the engine keeps a render-side palette/OAM copy the journal feeds; the guest bytes stay live, so no copy-on-write; silent stores dropped in `Gpu::palette_store` / `oam_store`; palette/OAM pages are permanently slow-path for stores (~400/frame) |
| 4.16 | VRAM bank remaps deferred to next render | 04 §5 | equiv | not deferred: `update_vram` catches the render up to the current line, remaps, re-arms the trap, and the frame stays batched |
| 4.17 | Geometry commands logged, replayed once at VBlank | 04 §6 | no | `Gpu3D::run_to` executes queued commands after every ARM9 slice (slice-granular, not frame-granular); decided against — the per-slice dispatch is an inline idle test and the games pace the engine by the FIFO (summary item 3); the enqueue path is one ring instead |
| 4.18 | Vertex transform as a batched kernel after replay | 04 §6 | no | vertices transformed as commands execute; a NEON form of the per-vertex transform measured worse than the scalar `smull`/`smaddl` chain (110 → 118 insn), so batching is the only route and it needs the replay model |
| 4.19 | `GXSTAT` (and FIFO IRQ/DMA) computed by replaying the log on demand | 04 §6 | same | register reads call `run_to` first; `swap_pending()` feeds idle skip — see Dragon Ball GXSTAT poll note |
| 4.20 | 3D render kicked at line 215, joined at VBlank | 04 §6 | same | `render_frame` at VCount 215, joined by `sync_line` as display reads each band |
| 4.21 | Audio buffer occupancy as primary frame limiter | 04 §7 | same | `Audio::pace()` sleeps above the target queue depth; wall clock only with `--no-audio` |
| 4.22 | Frame skip drops rendering only (CPU/geometry/SPU still exact) | 04 §7 | no | no frame skip at all |
| 4.23 | All thread hand-offs via mutex + condvar, no spinning | 04 §8 | equiv | LineWorker spins then parks; 3D workers mutex+condvar — see qemu LineWorker hang note |

## 05 — SPU

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 5.1 | Mix once per frame at VBlank | 05 §1 | partial | `ev_mix` mixes a batch of 16 samples per event (~34/frame, `DS_SPU_BATCH`); a batch of one while capture is on (it writes RAM); register accesses run the mixer up to now() first |
| 5.2 | Fixed-point cycle→sample conversion with carried remainder | 05 §1 | n/a | native 32.768 kHz, no rate conversion in the core |
| 5.3 | Catch-up mix on the audio driver's timer overflow (ARM7 timer 1) | 05 §1 | n/a | mixing is per sample |
| 5.4 | Resampling folded into playback: 32.32 cursor, nearest sample, no intermediate mix | 05 §2 | no | hardware-exact per-channel timers stepped per sample; SDL resamples the 32 kHz output |
| 5.5 | Pre-multiplied `vol_l` / `vol_r` per channel; one MAC per side per sample | 05 §2 | partial | `volume`/`vol_shift`/`pan` pre-decoded in `set_cnt`; pan applied in `mix()` per channel per sample |
| 5.6 | Source resolved to a host pointer at key-on; silent if not direct-mapped | 05 §2 | no | 32-byte per-channel FIFO refilled through the bus (hardware-exact, melonDS-comparable) |
| 5.7 | Register writes set per-channel dirty bits; resolved at next mix | 05 §3 | no | `set_cnt` decodes eagerly (cheap; no divides involved) |
| 5.8 | Only key-on is eager; busy bit cleared in the mirrored I/O word | 05 §3 | partial | key-on eager; busy bit lives in `cnt` |
| 5.9 | ADPCM decoded one word (8 samples) at a time into a 64-entry ring | 05 §4 | no | `next_adpcm` decodes one nibble per sample |
| 5.10 | Loop-point predictor/index snapshot; no re-decode on loop | 05 §4 | same | `adpcm_val_loop` / `adpcm_idx_loop` |
| 5.11 | PSG as 8-entry duty tables, noise as precomputed 32 K LFSR table | 05 §5 | no | LFSR stepped and PSG computed per sample |
| 5.12 | Clamp/narrow as auto-vectorised C over the whole buffer | 05 §6 | n/a | clamp per sample in `mix()` |
| 5.13 | Lock-free 64 K-sample ring with 16-bit indices to the audio thread | 05 §6 | same | 16 K-frame ring, `take()` from the frontend |
| 5.14 | Capture units emulated for timing/control only (data is silence) | 05 §6 | no — by choice | capture implemented for the mixer-output mode; add/channel modes warn — accuracy trade — decide, don't copy |
| 5.15 | Channels 0–3 keep a side sample for SOUNDCNT output select | 05 §6 | same | channel 1/3 bypass in `mix()` |
| 5.16 | `audio_sync` blocks at VBlank above ¾ buffer occupancy | 05 §7 | same | `Audio::pace()` |

## Cross-cutting

| # | Principle | Where it shows up |
|---|---|---|
| X.1 | Move every decision to the coarsest granularity at which it is still correct | per frame (SPU, geometry, 2D batch), per bin, per polygon, per 256-px batch, per block |
| X.2 | Replace a conditional test with a larger static table | page table, ITCM tables, branch cache, DMA cycle tables, noise table |
| X.3 | Record the request, do the work later, replay on observation | 2D journal, geometry log + `GXSTAT`, timers, dirty bits |
| X.4 | Silent-store elimination wherever a write can invalidate something | SMC, palette journal |
| X.5 | Specialise the kernel rather than branch in the loop | 133 3D kernels, 2D window variants, capture variants, LDM/STM helpers |
| X.6 | Schedule for an in-order core: independent chains, software pipelining | texel gather, vector loops |
| X.7 | Keep a C reference beside every hand-written kernel | 3D, 2D |
| X.8 | Apply assembly only where the profile justifies it | `obj_c`, the whole SPU |
| X.9 | Measure on the device, and distrust the emu-only total | 00; scanline-scaling and display-path notes |
