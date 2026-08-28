# Implementation checklist

A compressed index of every high-level technique in documents 01–05, one line
each, for auditing DSperate against DraStic. Each row names the technique,
the DraStic document that describes it, and a column for our status.

Fill the **DSperate** column with one of: `same` (we do this), `equiv` (we
achieve the same effect a different way — say how in a note), `partial`,
`no`, or `n/a` (does not apply to our design — say why). The point is not to
copy DraStic row by row; it is to make sure every deliberate *omission* is a
decision rather than an oversight. Rows that our own measurements have
already settled carry a note.

## 01 — Recompiler

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 1.1 | Guest r0–r14 pinned to host registers for the whole run | 01 §1 | | |
| 1.2 | Guest r6–r14 in the AAPCS64 callee-saved range so helpers spill 6 not 15 | 01 §1 | | |
| 1.3 | Guest CPSR flags live in host NZCV | 01 §1 | | |
| 1.4 | One register doubles as page-table base and helper spill base | 01 §1 | | |
| 1.5 | Two-instruction block prologue (`TBZ` on cycle sign bit); second entry point skips it | 01 §2 | | |
| 1.6 | Cycle cost accumulated per block, one `SUB` emitted | 01 §2 | | |
| 1.7 | Downward cycle counter tested by sign, not compared | 01 §2 | | |
| 1.8 | Per-CPU cycle model (ARM7 ×2, ARM9 + adjustment) with per-game hacks | 01 §2 | | |
| 1.9 | Direct linking of static branches, delayed patch list for untranslated targets | 01 §3a | | |
| 1.10 | Link may target the check-free entry; spin-loops forced out via `mov w12,#-1` | 01 §3a | | |
| 1.11 | 1024-entry inline direct-mapped branch-target cache in the CPU struct | 01 §3b | | |
| 1.12 | Tag-free direct tables for ITCM (ARM 8192 / Thumb 16384 slots) | 01 §3c | | |
| 1.13 | 32-bit cache-relative offsets for all block/metadata pointers | 01 §3b | | |
| 1.14 | Flat 2 KB-page table per CPU (16 MB), pre-biased base, entry stored `>>2` | 01 §4 | | |
| 1.15 | Zero entry covers unmapped, I/O and side-effect pages in one `cbz` | 01 §4 | | |
| 1.16 | Store path flag bits: 62 = slow path, 63 = code page | 01 §4 | | |
| 1.17 | Loads never test the flag bits | 01 §4 | | |
| 1.18 | SMC filter 1: code-page bit test | 01 §5 | | |
| 1.19 | SMC filter 2: silent-store elimination (compare before invalidate) | 01 §5 | | done — see JIT SMC filters note |
| 1.20 | SMC filter 3: block-range allocation check | 01 §5 | | done — see JIT SMC filters note |
| 1.21 | Self-invalidating block recovers PC from metadata, flushes, re-enters | 01 §5 | | |
| 1.22 | Compressed host-offset → guest-PC map per block | 01 §5 | | |
| 1.23 | Three code arenas (ITCM / main RAM / other) with separate flushes | 01 §6 | | |
| 1.24 | Code grows forward, metadata backward in one arena | 01 §6 | | |
| 1.25 | Direct opcode emission, no IR | 01 §7 | | |
| 1.26 | Per-register known-constant tracking with `MOVZ/MOVK/ORR`-imm materialisation | 01 §7 | | |
| 1.27 | Intra-block forward-branch fix-up pass | 01 §7 | | |
| 1.28 | Sixteen specialised LDM/STM helpers per direction | 01 §7 | | |
| 1.29 | GXFIFO / geometry ports recognised inline in the ARM9 32-bit store helper | 01 §8 | | done — DMA→GXFIFO dispatch bypassed; see dbori note |

## 02 — 3D rasteriser

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 2.1 | 12 bins × 16 scanlines; ~32 KB live tile footprint | 02 §1 | | measured: pays with 2 workers / 8 even bins — see binning note |
| 2.2 | Branch-free 12-bit bin mask from ymin/ymax | 02 §1 | | |
| 2.3 | Bins ÷ thread count for even split on 1/2/3/4/6/12 cores | 02 §1 | | adaptive worker controller instead — see note |
| 2.4 | AND/OR uniformity test per polygon selects `_constant` kernels | 02 §2 | | |
| 2.5 | Specialisation matrix: 9 wrap × 8 combine × 10 resolve × 4 depth | 02 §2 | | |
| 2.6 | Fused resolve (edge mark + fog + convert + store) chosen once per bin | 02 §2 | | |
| 2.7 | Spans batched to 256 px across scanlines before the pipeline runs | 02 §3 | | measured flat — see per-span-cost note |
| 2.8 | Per-256-px kernel selection; flush has 39 direct calls, no indirect | 02 §3 | | |
| 2.9 | Stage-at-a-time SoA span pipeline; conditionals become byte masks | 02 §3 | | |
| 2.10 | Per-span interpolants broadcast to per-pixel arrays, then one flat pass | 02 §3 | | measured: does not port (+0.6–1.7 %) — see 02 §3 |
| 2.11 | Constant-W perspective factor as iota ramp, no division | 02 §3 | | |
| 2.12 | Depth test emits mask + surviving count; later stages skip on zero | 02 §3 | | |
| 2.13 | Mask-shift tail handling, no scalar epilogue | 02 §3 | | |
| 2.14 | Eight-way unrolled scalar texel gather for in-order latency hiding | 02 §4 | | |
| 2.15 | Software-pipelined vector loops (load next before store current) | 02 §4 | | |
| 2.16 | Fixed point throughout, no FP | 02 §4 | | |
| 2.17 | Texture lookup hoisted to bin time, one-entry memo | 02 §5 | | |
| 2.18 | Opaque then translucent pass with per-tile polygon-ID stencil | 02 §6 | | |
| 2.19 | Rear-plane bitmap path with scroll | 02 §6 | | |
| 2.20 | Paired `_c` / `_asm` kernels, C as bit-exact reference | 02 §7 | | |
| 2.21 | `_1x` / `_4x` (hi-res) kernel families | 02 §2 | | |

## 03 — 2D engines

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 3.1 | 1-bit-per-pixel visibility masks; scanline = two NEON registers | 03 §2 | | |
| 3.2 | NEON movemask idiom (`and` + `addp` tree) for 4/8/12/16 bpp | 03 §2 | | |
| 3.3 | Bit-parallel priority encoder yielding first *and* second layer | 03 §3 | | |
| 3.4 | Separate OBJ first/second accumulators for forced-blend sprites | 03 §3 | | |
| 3.5 | `_single` encoder when blending is off | 03 §3 | | |
| 3.6 | Backdrop mask from accumulated coverage | 03 §3 | | |
| 3.7 | Pixel select 32 px/iteration; zero mask word skips the group | 03 §4 | | |
| 3.8 | Planar 6-bit channel split once per line; effects as byte ops | 03 §5 | | |
| 3.9 | `tbl` + `ld2`/`st2` as a 4bpp palette unit | 03 §6 | | |
| 3.10 | `bit` + `rev32`/`sli` branch-free H-flip, four tiles at a time | 03 §6 | | |
| 3.11 | Windows as mask ANDs, one kernel per active-window count | 03 §7 | | |
| 3.12 | Mosaic applied to pixels and mask before priority | 03 §7 | | |
| 3.13 | Blank-layer elimination before the encoder | 03 §7 | | |
| 3.14 | 3D output presented as an ordinary layer (visibility + alpha gather) | 03 §7 | | |
| 3.15 | Fused display-capture variants | 03 §7 | | |
| 3.16 | Assembly conversion only where profiling justified it (`obj_c` stays C) | 03 §8 | | |

## 04 — Scheduler, deferral, DMA, memory

> **HIGH PRIORITY: lazy 2D (rows 4.11–4.16).** DSperate renders both 2D engines synchronously at every HBlank (`Gpu::on_hblank` → `Gpu::draw_line`, `src/core/gpu/gpu.cpp`), with a worker dispatch/join per line. DraStic renders the frame in one batch at VBlank, journals mid-frame writes, and hands engine B to a worker once. This is the largest unexploited item on the list; see [04 §5](04-scheduler-deferral-and-memory.md).

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 4.1 | Delta-encoded fixed-slot event list; slice = head delta | 04 §1 | | |
| 4.2 | Both CPUs run the same slice; ARM7 at doubled cycle cost | 04 §1 | | |
| 4.3 | Forced task switch at next 128-cycle boundary on cross-CPU dependency | 04 §1 | | |
| 4.4 | Scanline as two events (3,072 + 1,188 cycles) | 04 §1 | | |
| 4.5 | Timer count derived on read; overflow as an event | 04 §2 | | |
| 4.6 | `irq_pending` precomputed at every IF-setting site | 04 §3 | | |
| 4.7 | `pending_actions` + alert thunks at block boundaries only | 04 §3 | | |
| 4.8 | Whole-transfer DMA over a 16-entry 8 MB region table | 04 §4 | | |
| 4.9 | DMA cycle cost from static seq/non-seq tables; completion as event | 04 §4 | | |
| 4.10 | Coarse (64 KB) / fine (2 KB) code bitmaps ORed over DMA destination | 04 §4 | | |
| 4.11 | HBlank DMA into VRAM forces 2D render catch-up first | 04 §4 | | |
| 4.12 | 2D rendered as one batch at VBlank when nothing changed mid-frame | 04 §5 | **no — HIGH PRIORITY** | `Gpu::on_hblank` → `draw_line` renders both engines every line (`src/core/gpu/gpu.cpp`) |
| 4.13 | Engine B rendered on a worker thread, engine A on main | 04 §5 | partial — **HIGH PRIORITY** | we dispatch/join the engine-B worker per line (192 hand-offs/frame); DraStic does it once per frame |
| 4.14 | Per-engine journal of mid-frame register/palette/OAM writes, replayed per line | 04 §5 | **no — HIGH PRIORITY** | prerequisite for 4.12; we apply writes directly and rely on per-line rendering for correctness |
| 4.15 | Copy-on-first-write shadow palette/OAM; journal only if value changed | 04 §5 | no | part of the 4.12–4.14 cluster |
| 4.16 | VRAM bank remaps deferred to next render | 04 §5 | | |
| 4.17 | Geometry commands logged, replayed once at VBlank | 04 §6 | | |
| 4.18 | Vertex transform as a batched kernel after replay | 04 §6 | | |
| 4.19 | `GXSTAT` (and FIFO IRQ/DMA) computed by replaying the log on demand | 04 §6 | | see Dragon Ball GXSTAT poll note |
| 4.20 | 3D render kicked at line 215, joined at VBlank | 04 §6 | | |
| 4.21 | Audio buffer occupancy as primary frame limiter | 04 §7 | | |
| 4.22 | Frame skip drops rendering only (CPU/geometry/SPU still exact) | 04 §7 | | |
| 4.23 | All thread hand-offs via mutex + condvar, no spinning | 04 §8 | | see qemu LineWorker hang note |

## 05 — SPU

| # | Technique | Ref | DSperate | Note |
|---|---|---|---|---|
| 5.1 | Mix once per frame at VBlank | 05 §1 | | |
| 5.2 | Fixed-point cycle→sample conversion with carried remainder | 05 §1 | | |
| 5.3 | Catch-up mix on the audio driver's timer overflow (ARM7 timer 1) | 05 §1 | | |
| 5.4 | Resampling folded into playback: 32.32 cursor, nearest sample, no intermediate mix | 05 §2 | | |
| 5.5 | Pre-multiplied `vol_l` / `vol_r` per channel; one MAC per side per sample | 05 §2 | | |
| 5.6 | Source resolved to a host pointer at key-on; silent if not direct-mapped | 05 §2 | | |
| 5.7 | Register writes set per-channel dirty bits; resolved at next mix | 05 §3 | | |
| 5.8 | Only key-on is eager; busy bit cleared in the mirrored I/O word | 05 §3 | | |
| 5.9 | ADPCM decoded one word (8 samples) at a time into a 64-entry ring | 05 §4 | | |
| 5.10 | Loop-point predictor/index snapshot; no re-decode on loop | 05 §4 | | |
| 5.11 | PSG as 8-entry duty tables, noise as precomputed 32 K LFSR table | 05 §5 | | |
| 5.12 | Clamp/narrow as auto-vectorised C over the whole buffer | 05 §6 | | |
| 5.13 | Lock-free 64 K-sample ring with 16-bit indices to the audio thread | 05 §6 | | |
| 5.14 | Capture units emulated for timing/control only (data is silence) | 05 §6 | | accuracy trade — decide, don't copy |
| 5.15 | Channels 0–3 keep a side sample for SOUNDCNT output select | 05 §6 | | |
| 5.16 | `audio_sync` blocks at VBlank above ¾ buffer occupancy | 05 §7 | | |

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
