# DSperate performance audit for RK3566-class devices (2026-09-07)

Six subsystem audits (JIT/CPU, system glue, 3D, 2D, frontend, peripherals+build), each read the code and quoted lines. Full per-subsystem reports follow this summary. Everything below was checked against the measured dead-ends list (fastmem, branch following, adaptive workers, span batching, sixteen-lane kernels, untimed DMA, A32 subset, etc.) and none of those are re-proposed.

## Executive ranking

Tiers are by expected win per unit of effort, with "bit-exact" meaning frame hashes must not move.

### Tier A — cheap, bit-exact, directly aimed at the measured bottleneck (frontend stalls, I-cache)

| # | Item | Where | Win (honest) | Effort |
|---|------|-------|--------------|--------|
| A1 | Build flags: `-D_FORTIFY_SOURCE=0`, `-fno-stack-protector`, `-mtune=cortex-a55` (or `-mcpu` gated per device). The AArch64 build is generic ARMv8.0 with distro hardening on; `__memcpy_chk` sits inside the sprite fetch loop. The fl-* build trees from 2026-08-30 were never timed on device. | CMakeLists.txt:64-73 | 2-6 % main thread | hours; ONE A/B, both run orders |
| A2 | PGO: `-fprofile-generate -fprofile-update=atomic` trained on the five headless replays (with real BIOS/firmware, --quantum 0), then `-fprofile-use`. The only lever that attacks the 44 % frontend-stall / IPC 0.29 number directly. | CMake + tools/scene_hashes.sh | 3-10 % | 1-2 days + CI story |
| A3 | JIT arena: cold sections are spliced *after* each block's hot code, so the layout is hot/cold/hot/cold and ~1/3 of streamed I-cache bytes never execute. Give cold code its own frontier. `[jit] code N KB (hot M KB)` in report() already sizes it. | translate.cpp:431-444, runtime.cpp:468-471 | 0.2-0.6 ms GSDD | medium |
| A4 | Stub attribution: `perf script -F symoff` over the named stubs decides whether branch_indirect, merge_keep_cv or call_pure owns the 0.59 M stub insn/frame. Do before A5/A6. | runtime.cpp perf_map_stubs | measurement | afternoon |

### Tier B — p99 / experience levers in the frontend (bit-exact, presentation only)

| # | Item | Where | Note |
|---|------|-------|------|
| B1 | DrmOut has BUFS=2 and waits for the previous flip to retire unconditionally: zero jitter absorption, an 18 ms frame presents at 33 ms. Go to 3 buffers, block only when none free (DmabufOut already does this). Dual-window serialises two vblank waits. | display_drm.h:44, display_drm.cpp:220-233 | p99/over-budget, not mean |
| B2 | Every thread inherits SCHED_RR prio 5 (emu, LineWorker, 2-3 band workers, SDL audio, SDL event) on 4 cores; nothing pinned; sway is SCHED_OTHER. Audio thread (the frame clock) can starve; compositor can only run via the RT throttle (50 ms hiccup). Try `chrt` before code. | main.cpp:697-706, rt_thread.h | prior RT change moved Golden Sun p99 23→18 |
| B3 | Input polled ~330 lines before the vsync wait in begin_frame: one full frame of stale input on scanout tiers. Re-poll after begin_frame. | main.cpp:1357, 1692 | latency, not fps |
| B4 | The scanout wait sits outside frame_ms/work_ms (t0 taken after begin_frame), so DS_FPS under-reports the shipping tiers; headless never sets a scale target so headless emu numbers exclude scaling. Add wait_ms. | main.cpp:1698 | instrument fix |
| B5 | Audio::pace busy-polls SDL_GetQueuedAudioSize + SDL_Delay(1) under the device lock; drain rate is known, sleep once. | audio.cpp:154-167 | small |
| B6 | Translucent PiP inset reads back WC scanout memory per pixel whenever pip_alpha<1. Shadow the rect. Verify with a pip_alpha A/B first. | display.cpp:744-775 | ~1 ms if enabled |

### Tier C — system glue (exact unless noted)

| # | Item | Where |
|---|------|-------|
| C1 | Every VRAMCNT byte write = full VRAM remap (~128 KB memset + ~128 KB page-table RMW) + trap re-arm + sync_raster + worker join; a 32-bit store pays it four times. Defer/coalesce (DraStic does, techniques/04 §5). | io.cpp:1168, bus.cpp:144, gpu.cpp:235 |
| C2 | set_write_trap walks 4096-8192 entries per arm/disarm (~31 toggles/frame on GS ≈ 2 MB/frame) when only ~330 pages are mapped. Keep a list. | page_table.cpp:143 |
| C3 | Io::read16/write16 are a 55-branch binary search over sparse 32-bit addresses; 32-bit reads walk it twice; write8 is RMW through read16+write16. Switch on addr&0xFFF or a handler-id table. Size with DS_IO_CENSUS. | io.cpp:937/1019 |
| C4 | `prof::enabled` load + branch inside PageTable::write_ptr on every store path. Make compile-time. | page_table.h:129 |
| C5 | ipc_sync_write yields the slice unconditionally (comment says boot handshake only). NOT exact; measure slice delta first. | io.cpp:121 |
| C6 | update_tcm rebuilds WRAM+VRAM (raster join) on any TCM change; DMA fill (src_inc==0) never hits the bulk path; CpuContext hot fields 250 B past JitHot; slow_accesses++ global RMW in six JIT helpers. | bus.cpp:272, dma.cpp:325/409, runtime.cpp:800+ |

### Tier D — 3D rasteriser (all bit-exact)

| # | Item | Where | Est. |
|---|------|-------|------|
| D1 | Every band worker rebuilds the whole ~490 B-per-Edge array for every polygon (~1 MB per Renderer3D instance, ~1.9 MB stores/frame on GSDD), evicting the 25 KB tile ring. Build edges lazily on first use. | render3d.cpp:3013 build_edges | 1.5-4 % raster GSDD |
| D2 | mul_hi8_add widens to 64-bit (vmull_u32, 2 lanes/insn) for interpolants that fit 32 bits; colour fits 16. Narrow with a per-span vmaxq guard and fallback. | kernels_neon.cpp:907 | 1-3 % etody |
| D3 | Interp::set_x does a udiv per scanline per edge even when the denominator is constant; precompute reciprocal as interpolate() already does. | render3d.cpp:54 | small, on the edge-walk chain |
| D4 | Profiling flag reloaded inside the innermost resolve loop (2 global loads + branches per 8 px); scalar resolve_span counts per pixel. stage_line already hoists it. | render3d.cpp:1599, 1631, 949 | 2-4 % of resolve |
| D5 | depth_candidates_m: two narrowing moves, redundant vcombine, 4-byte lane store, scalar byte scan per 4 px; unroll to 16 with narrow(). | kernels_neon.cpp:1252 | 0.5-1.5 % |
| D6 | Smaller: occluded span head/tail still gathered+shaded; untextured polys never batch (stale reasoning); no one-entry texcache memo; xcov_incr computed with AA off; edge-mark fetches a byte plane via four vld4q; job_fn_ std::function allocated per frame. | render3d.cpp:1884, 2129 | |

### Tier E — 2D engines (all bit-exact; gate with scene_hashes + DS_DEBUG_OUTHASH)

| # | Item | Where | Est. |
|---|------|-------|------|
| E1 | Measure DS_2D_SPLIT=1 on GSDD: it exists, is exact, defaults off; without it every VRAM store bursts both engines, which is the shape of the "DS_2D_LAZY=0 wins 5.4 %" result. Zero code. | gpu.h:319 | maybe most of the 5.4 % |
| E2 | rgb555_table is a 128 KB gather table installed per line and gathered scalar per pixel against a 32 KB L1D; the conversion is 6 ALU ops and vectorises. Delete it. | engine2d.cpp:431, 795, 1244 | best value/effort |
| E3 | text_row_16/256 do one tile per iteration with three GPR→SIMD dups; go 2-4 tiles with a vector ctl load + vtstq. Hottest 2D kernel. | kernels_neon.cpp:352, 370 | 25-40 % of that kernel, 0.2-0.5 ms |
| E4 | draw_bg_text's 33-tile gather: cache vv.ptr[block], dead palmask, 32 scalar compares for uniform. | engine2d.cpp:645-694 | 0.1-0.25 ms |
| E5 | composite_line pass-through pays ld4/st4 just to force alpha; vmaxvq+fmov used as scalar predicates (compat::uniform_u8 exists, no callers); resolve16_one round-trips indices through the stack (store-forward stall 32x/line). | kernels_neon.cpp:139, 246, 308, 493, 508 | 0.1-0.3 ms each |
| E6 | Unconditional per-line clears (~2-3 KB/line/engine): three sprite planes only needed under OA_OPAQUE, win_ fill on no-window lines, 512 B memset before a full-coverage degenerate path (GSDD engine B), second-target arrays. | engine2d.cpp:989, 1192, 782/854, 1281 | |
| E7 | Sprite row assembly scalar (2-byte memcpy per pixel, loop-carried hflip branch); rotscale sprites use out-of-line VramMap::read8 instead of the inline fetch. | engine2d.cpp:1067-1165 | |
| E8 | Per-line magic-static guards (ablate() 3-4x/line/engine, rgb555_table per line, getenv statics in join_worker/render_ranges, 3 getenv per frame in begin_frame). | gpu.cpp:31 | small |
| E9 | Longer term: a 32-bit tile-occupancy mask returned from text_row_*/bmp_row_* lets merge16 skip 8-px groups with a tbz (down payment on DraStic's bitmask priority encoder). | kernels_neon.cpp:396 | |

### Tier F — JIT structural (after A4 says which stub matters)

| # | Item | Where |
|---|------|-------|
| F1 | branch_indirect: ~25 insns of ARM9 refill-cost arithmetic with two dependent timing-table byte loads on every BX/POP pc. Precompute per-page like cost7. Size with DS_CENSUS indirect-branches/frame first. | stubs.cpp:433-492 |
| F2 | Flags assumed fully live at block end, so most S-form instructions `bl` merge_keep_cv (two mrs + msr nzcv). 3b: inline it (+5 words/site, weigh against A3). 3a: inter-block liveness via Block::succ, needs a new predecessor-invalidation edge (risk). | translate.cpp:544-547, 1808-1814; stubs.cpp:266 |
| F3 | Page-table entry to 4 bytes (all backing in one reserved window): halves 16 MB/CPU table, and makes the stashed timing fold free at today's footprint. Not fastmem. High effort. | page_table.h:53-79 |
| F4 | LUT_BITS 16 = 1 MB of LUT on a 512 KB L3 next to three streaming workers; re-A/B at 13 under p99. | jit_internal.h:305 |

### Tier G — peripherals / cliffs (not benchmark scenes, but real users)

- SPU mixes all 16 channels per sample; add an active mask (exact, half the cost for an hour), channel-outer batching later. spu.cpp:363
- Cheat engine walks the whole ~88 B/entry code list on every ARM7 VBlank IRQ, more than once a frame; keep an enabled list. cpu.cpp:163, ar_engine.cpp:214
- Cart::rom_read32 assembles words byte by byte; memcpy unless in the last 3 bytes of a page. cart.cpp:168

## Bugs found on the way

1. DMA save state drops the WRITE16/WRITE32/WRITE32_N2 burst tables (restored as MRAM_DUMMY): wrong DMA timing after a load. dma.cpp:519
2. Dual-window: if display.begin_frame succeeds and display2's fails, display's end_frame never runs, surface stays locked, frame_px_ dangles. main.cpp:1692-1693
3. Dead emit_fetch_cost9 call (6 insns + a load) then movz w4,#0 in branch_indirect_cdi's Thumb-even path; a dead add_imm two lines later. stubs.cpp:376-387
4. The profiler heap-allocates a prof::Scope per line (~1150 malloc/free per frame under DS_PROFILE): part of the recorded +2.9 ms instrument cost and it skews the breakdown. engine2d.cpp:475, gpu.cpp:723, 735
5. Spu::sync_state saves batch_ but not cap_batch_. spu.cpp:400
6. No DMA_BUF_IOCTL_SYNC anywhere; correctness depends on the dma-heap mapping being uncached. Verify on the RK3566 via smaps; log which heap was chosen.
7. output_a case 1 unreachable (mode 1 goes to the fused kernel). gpu.cpp:1136
8. run_until_impl and slice_next are two hand-kept copies of one state machine. scheduler.cpp
9. build/asan is -O3 -DNDEBUG -flto + sanitizers: not testing what its name says.
10. translate.cpp:11-18 claims "the I-cache holds only what runs"; false at arena level (see A3).
11. Fidelity notes: span_factor's constant-W ramp is a known inexact path with no switch to the exact one; AUXSPI sector erase is instantaneous (io-busy-bits-by-time did not cover it).

## Closed leads

- The 78 env knobs are NOT a hot-path problem: all 121 getenv sites are load-time or once-per-frame statics. Close the audit item.
- JIT block dispatch/lookup is NOT the lever: ~30 k of 34 k transitions are a patched `b`; the dispatcher runs 4.5 k times/frame ≈ 3 % of host insns.
- The ARM7 interpreter is not a shipping path (both CPUs are JITed in the SDL frontend).
- No virtuals in core, one std::function, no exceptions in hot code, LSE irrelevant, LTO real on both targets.

## Suggested first week

1. A1 as one A/B on .20 (dbori mean, etody+GSDD p99, both run orders, md5 both binaries).
2. E1 (DS_2D_SPLIT=1 on GSDD) and B2 (chrt experiment) — zero code, same rig session.
3. A4 (stub histogram) and the [jit] hot/code ratio from an existing report() line.
4. Then E2, D4, C4, E8, bug 4: all trivial, all exact, remove instrument bias before the bigger items.
5. A3, B1, D1, C1/C2, E3 as the first real changes, in that order.
6. A2 (PGO) once the build is stable, since it re-baselines everything.

----------------------------------------------------------------
# FULL REPORT: jit

# CPU emulation path (AArch64 JIT + interpreter) — audit

Scope read in full: `src/core/cpu/jit/a64/{translate.cpp,stubs.cpp,emit.h,convention.h}`,
`src/core/cpu/jit/{runtime.cpp,jit.h,jit_internal.h,block_shape.h}`,
`src/core/cpu/{cpu.cpp,cpu_cycles.h,cpu_mem.h,arm_decode.h}`, `src/core/cpu/interp/*`,
`src/core/mem/{page_table.h,timing.h}`, `docs/techniques/01-arm-to-aarch64-jit.md`.

## Facts established before ranking

**ARM7 is JIT'ed, not interpreted.** `src/frontend/sdl/main.cpp:1131` calls
`jit::attach(nds, true, true)`; headless takes `jit9`/`jit7` separately
(`headless/main.cpp:284`). `interp::run` is reached only via `cpu.step_limit`
(`runtime.cpp:1054`) and per-instruction through `jit_h_fallback`
(`runtime.cpp:741`). So the "ARM7 interpreter hot loop" is not a shipping hot
path; its cost shows up only as the ~2.8 k fallbacks/frame already priced at
166 ns each. I found nothing worth fixing in `interp.cpp`'s loop that is not
already dominated by that pricing, and I do not propose anything there.

**Dispatch is smaller than the open-leads list assumes.** ~34 k block
entries/frame vs 4.5 k dispatcher entries/frame (the memory notes' own
counters) means ~30 k of 34 k block transitions are already a single patched
`b` (`stubs.cpp:510 patch_link`, `translate.cpp:712 call_stub(jc_.link)`).
Conditional branches are handled well: `TOp::BCond` (`translate.cpp:1741-1750`)
emits **both** edges as linked static branches, so there is no indirect branch
for a guest `bcc`. The dispatch stub itself is 7-8 instructions
(`stubs.cpp:292-303`) and runs 4.5 k times a frame ≈ 55 k host insns — ~3 % of
the 1.88 M. **Block lookup/hash/return-to-dispatcher is not the lever.** What
*is* left in the 0.59 M stub instructions per frame is, by elimination,
`branch_indirect` (every guest `BX`/`POP pc`/`LDM pc`), the two flag-merge
stubs, and `call_pure`/`call_full` around slow accesses and fallbacks.

**The binding constraint is L1I footprint, and the code confirms it.** 44-45 %
frontend stalls, 26-35 k L1I refills/frame in ARM9 bodies, 5.88 MB of hot code
streamed per frame into a 32 KB L1I, against DraStic's ~22 k with *more*
instructions. Items 1-3 below all attack bytes-streamed rather than
instructions-issued, which is the axis the PMU says is live.

---

## Ranked opportunities

### 1. Cold sections are spliced *between* hot blocks, so ~1/3 of every I-cache line stream never executes

`translate.cpp:11-18` claims "the hot code of a block is contiguous and the
I-cache holds only what runs". The first half is true; the second is not.
`Translator::finish()` copies the cold buffer immediately after the hot code
**in the same block allocation**:

```cpp
// translate.cpp:431-444
const size_t cold_base = hot_.size();
blk_.hot_size = static_cast<u32>(cold_base);
...
std::memcpy(hot_.cur(), cold_buf_, cold_.size());
hot_.set_pos(cold_base + cold_.size());
```

and `translate()` then bumps the arena frontier by the *whole* block:

```cpp
// runtime.cpp:468-471
b->entry = r.arena + r.pos;
b->size  = size;                       // hot + cold
r.pos += (b->size + 15) & ~size_t{15};
```

So the layout is `[hot A][cold A][hot B][cold B]...`. Blocks are 4-5 guest
instructions ≈ 40 host instructions ≈ 160 B hot, and every memory access
contributes a cold path of roughly 10-15 words (`emit_single`'s
`cold_begin(fail)` arm at `translate.cpp:971-979`: mov, `bl slow_load`,
`emit_load_post`, `cost()`, `charge()`, `bl poll`, a literal word, and the
jump back), plus `emit_budget_check`'s cold exit
(`translate.cpp:513-520`: `bl exit_key_lit` + literal) and every
`emit_block_transfer` fallback arm (`translate.cpp:1046-1048`). On an A55 the
L1I fetches 64 B lines, so a block's cold tail is pulled in with the next
block's entry on essentially every line boundary.

**Why it costs:** the frontend streams `code_bytes`, not `hot_bytes`. The
runtime already measures the ratio — `report()` prints
`[jit] code N KB (hot M KB)` (`runtime.cpp:945-947`) — so the size of this is
readable from any existing run without new instrumentation.

**Change:** give the arena a second frontier for cold sections (e.g. cold grows
downward from the top of the block region, hot upward), so hot sections of
consecutively-translated blocks are adjacent. Everything needed already exists:
cold code is written to a side buffer and every cross-section branch is already
a recorded `Fix` resolved at splice time (`translate.cpp:385-412, 440-443`).
The only new constraint is branch range — `BLOCK_LIMIT = 28 KB`
(`translate.cpp:48`) exists to keep cross-section `tbnz` in range (±32 KB), so a
split frontier needs either a `tbnz`→`b.cond`/`b` trampoline for the
hot→cold edges or a bounded cold region per arena chunk. `b`/`bl` (±128 MB)
covers the cold→hot return and cold→stub edges in a 64 MB arena unchanged.

*Exactness:* bit-exact — pure layout; no emitted semantics change.
*Size:* honest guess 0.2-0.6 ms/frame on GSDD. Frontend stalls are 44 % of
~1.8 ms of body cycles; a ~30 % cut in bytes streamed will not cut stalls 30 %,
but this is the cheapest available reduction of the exact quantity the PMU
blames. *Measure:* GSDD title (`scenes/gsdd-phase2.dss`) and etody, `--quantum 0`,
p99 + over-budget frames; confirm with `L1I_CACHE_REFILL` on the ARM9-body
symbols via `DS_PERF_MAP=1` before/after.
*Risk/effort:* medium effort (arena bookkeeping, `Block::size`/`hot_size` and
`kill_block`'s entry patch are unaffected; `perf_map_add` ranges need updating),
low risk — a frame-hash gate catches any fixup mistake immediately.

---

### 2. `branch_indirect` runs ~25 instructions of ARM9 refill-cost arithmetic on every guest return

Every `BX`, `BLX reg`, `MOV pc,..`, `POP {..,pc}` and `LDM {..,pc}` funnels
through `jc.branch_indirect` (`translate.cpp:719-728` → `stubs.cpp:433-492`).
The ARM9 arm is:

```cpp
// stubs.cpp:454-475 — after the CPSR T-bit update
e.and_imm(4, 0, 1); e.ubfx(5, 2, 1, 1); e.sub_reg(3, 2, 5, LSL, 1);
e.lsr_imm(3, 3, 12); e.add_reg(3, R_TIM, 3, LSL, 3, true); e.ldrb(3, 3, 0);
e.movz(6, 3); e.cmp_imm(3, 0xFF); e.csel(3, 6, 3, EQ);
e.add_imm(7, 2, 4); e.sub_reg(7, 7, 4, LSL, 1);
e.lsr_imm(1, 7, 12); e.add_reg(1, R_TIM, 1, LSL, 3, true); e.ldrb(1, 1, 0);
e.tst_imm(7, 0x1F); e.movz(7, 1); e.csel(7, 6, 7, EQ);
e.cmp_imm(1, 0xFF); e.csel(1, 7, 1, EQ);
e.bic_reg(4, 4, 5); e.cmp_imm(4, 0); e.csel(1, ZR, 1, NE);
e.add_reg(3, 3, 1);
```

That is 22 instructions containing **two dependent byte loads from the timing
table** (each `lsr; add; ldrb` is a load-use chain into a `cmp`/`csel` on an
in-order core), plus `mrs`/`msr nzcv` around it (`stubs.cpp:434, 491`) and the
CPSR load-modify-store (below). Add the dispatch probe and the total per guest
return is ~35 host instructions with 3 loads and 1 store. DraStic's whole
indirect-branch path is nine instructions and two loads
(`docs/techniques/01-arm-to-aarch64-jit.md:150-166`), and it is still their
single largest JIT symbol at 1.56-1.63 % of cycles — so this path is worth
roughly that much on our side too, *times* the ~3x instruction ratio.

**Change:** precompute the refill cost per 4 KB ARM9 page, exactly as the ARM7
data cost is already precomputed. `mem::Timing` already does this trick for the
ARM7 (`timing.h:36-52` and `Translator::cost7_slot`, `translate.cpp:820-825`,
where the comment says it "replac[es] a mispredictable branch over up to
fourteen instructions"). `refill_cycles` (`cpu_cycles.h:105-120`) has exactly
three address-dependent inputs on the ARM9: the page, `thumb`, and the two bits
`addr&2` / `addr&0x1F`. A 4-byte-per-page side table indexed
`[thumb][odd]`, plus one inline `tst #0x1F` correction that is only needed on
cacheable pages, collapses the block above to ~6 instructions and one load.
Note `cost7()` is deliberately placed at `COST7_OFFSET` past the pinned raw
table so `R_TIM` still reaches it with one add (`timing.h:44-51`) — do the same
for the ARM9 table and `R_TIM` needs no change.

*Exactness:* bit-exact by construction if the table is built by calling
`refill_cycles` itself, exactly as `build_cost7` does. The retime dependency
machinery (`Block::dep_*`, `Timing::RETIME_CODE`) already covers the pages
involved.
*Size:* proportional to indirect branches/frame — `DS_CENSUS=1` already prints
`indirect branches/frame` (`interp.cpp:147`), so size this **before** building
it. Guess 0.1-0.3 ms on GSDD/sm64.
*Measure:* etody and sm64 (return-heavy game code) rather than GSDD, whose
main thread is DMA/GX-dominated; `--quantum 0`, p99.
*Risk/effort:* medium; the ARM9 refill rule has three shapes and the current
branch-free form is subtle. Gate with `tools/scene_hashes.sh` on all five scenes.

---

### 3. Flags are assumed fully live at every block end, so most flag-setting instructions pay a `bl` to a merge stub

`Translator::run` seeds the backward liveness pass with everything live:

```cpp
// translate.cpp:1808-1814
u32 live = F_ALL;
for (size_t i = instrs_.size(); i-- > 0;) {
  instrs_[i].live_out = live;
  ...
}
```

Blocks are 4-5 guest instructions (`block_shape.h` cuts at every branch, and
branch-following was measured flat and reverted), so the *last* flag-setting
instruction in a block — very often the only one — always sees `live_ == F_ALL`.
`set_flags_logical` then takes its most expensive arm:

```cpp
// translate.cpp:544-547
if (!need_c || keep_c) {   // C kept (or dead), V kept
  if (res != SCRATCH0) e().mov(SCRATCH0, res);
  call_stub(rt().merge_keep_cv);
  return;
}
```

`merge_keep_cv` (`stubs.cpp:266-273`) is `mrs; tst; mrs; ubfx; bfi; msr; ret` —
so a guest Thumb `MOVS r0,#imm` or `ANDS`/`ORRS`/`BICS`/`MVNS`, and every ARM
logical `S` form, costs a `bl` + 6 instructions + `ret`, **including two `mrs
nzcv` and one `msr nzcv`**, which on an A55 are among the slowest non-memory
ops and do not dual-issue. Contrast the cheap arm, `e().tst_reg(res, res)`
(one instruction), which is taken only when neither C nor V is live.

Two independent fixes, both worth doing:

**3a. Inter-block flag liveness.** The A32 phase-0 census already concluded
"build inter-block flag liveness first"
(`docs/arm32-jit-scoping.md` §6; `DensitySlot::entry_flags_live`,
`jit_internal.h:256`, exists precisely to size this). The machinery is in
place: `Block::succ[4]`/`nsucc` (`jit_internal.h:277-279`) records static
branch targets, and `DS_JIT_CENSUS` already measures `entry_flags_live`. A
conservative-but-useful version needs no interprocedural fixed point: when a
block's single static successor is already translated and does not read NZCV
before writing them, seed `live = 0`. Invalidation is already handled — a
successor that is killed/retranslated must kill its predecessors, which is a
new edge the runtime does not currently maintain, so this is the real cost of
the item.
**3b. Inline the merge instead of calling it.** Even at `F_ALL`, the sequence
is 6 instructions with no branches; calling it adds a `bl` and a `ret`, pulls
in a distant cache line, and consumes return-stack depth inside a block that
may itself have been reached by a `bl`. Inlining costs +5 words per site
against item 1's byte budget, so measure the two together.

*Exactness:* 3b is trivially bit-exact. 3a is bit-exact *if* the successor
edge is invalidated correctly — that is the whole risk.
*Size:* `DS_JIT_CENSUS` gives `entry_flags_live` and `n_flags_live` per
translation weighted by executions; read it first. If the census says most
blocks do not read NZCV on entry, 3a removes the merge stub from the common
case entirely.
*Measure:* sm64 and mlbis (guest-code-bound scenes), not GSDD.
*Risk/effort:* 3b low/low; 3a medium/high (a new invalidation edge, easy to
get subtly wrong — gate with `DS_JIT_STRICT` + `tools/scene_hashes.sh`).

---

### 4. Halve the page-table entry to 4 bytes — this also un-blocks the stashed timing fold at zero footprint cost

```cpp
// page_table.h:53-55, 78-79
constexpr u32 PAGE_SHIFT = 11;                     // 2 KB
constexpr u32 PAGE_COUNT = 1u << (32 - PAGE_SHIFT); // 2 Mi entries = 16 MiB of table
using Entry = uintptr_t;                            // 8 bytes
```

16 MB of table **per CPU**, on a part with 512 KB of L3 shared with three
raster workers streaming framebuffers. The PMU symoff histogram already names
`lsl x3, x2, #2` — the consumer of the page-table entry load
(`translate.cpp:939`) — as a top stall offset, which means these entries are
*missing*, not merely latent. `DS_JIT_MEMPROBE` explicitly cannot see that:
its duplicate load always hits the line the real one is about to touch.

The entry is `(host - addr) >> 2` plus two tag bits. All guest RAM backing is
already allocated `PAGE_SIZE`-aligned through one helper
(`alloc_page_buf`, `page_table.h:67-75`) and the DS has <16 MB of guest RAM, so
placing every backing buffer inside one reserved contiguous mmap window makes
the base fit in 30 bits + 2 tags. Emitted cost: **+1 add** per access if the
window base is pinned (x17 is free in block bodies — `convention.h:35` reserves
it as `R_FN` for the stubs only), or zero if the window is placed so the base
can be folded into `R_PT`'s addressing.

The payoff is compounding:
* Halves the resident page-table footprint and puts twice as many *adjacent
  guest pages* on one 64 B line (2 KB pages means sequential guest access walks
  sequential entries), which is exactly the miss pattern the histogram shows.
* **It makes the stashed timing fold free.** The fold was abandoned because the
  exact 16-byte slot layout doubled the table and the store-cost `ubfx`
  diverged. With a 4-byte base, an 8-byte slot holds base + 4 timing bytes at
  *today's* footprint, and the fold removes the `lsr; add; ldrb` triple that
  `emit_data_cost` (`translate.cpp:805-815`) emits on every single access —
  the census puts page-walk + cost at 35.7 % of all emitted host words.

This is **not** the ruled-out fastmem item: no mprotect, no backpatching, the
timing table and the walk both survive.

*Exactness:* bit-exact (representation change only), *provided* the timing fold
is re-derived rather than re-typed — that is where the previous attempt broke.
*Size:* the instruction-count half is small (memprobe priced the whole walk at
0.52 ms); the *miss* half is unpriced and is the reason to do it. Guess
0.3-0.8 ms on GSDD, with wide error bars.
*Measure:* PMU `L1D_CACHE_REFILL` / `L2D_CACHE_REFILL` attributed to the
`lsl x3,x2,#2` offsets via `DS_PERF_MAP=1`, on GSDD; then frame time p99.
*Risk/effort:* high effort (every `map`/`remap`/`unmap`/`set_code_host` path,
the reverse `HostIndex`, both the interpreter helpers in `cpu_mem.h` and the
emitted walk), medium risk. Do items 1-3 first.

---

### 5. Before any of 2/3: attribute the 0.59 M stub instructions per frame

`perf_map_stubs(r)` is already called at attach (`runtime.cpp:~865`) and names
every stub, so a `perf script -F symoff` histogram over the stub symbols is a
one-afternoon job that turns my elimination argument in "Facts" into a
measurement. It decides whether item 2 (`branch_indirect`), item 3
(`merge_keep_cv`/`merge_set_c`) or `call_pure`/`call_full` around the 4.6 k
slow accesses and 2.8 k fallbacks is the biggest of the three. The memory
notes call dispatch "the largest unpriced piece"; the counters in the same note
(4.5 k dispatcher entries vs 34 k block entries) say it is not, and this
settles it either way.
*Effort:* trivial. *Do this first.*

---

### 6. Re-measure `LUT_BITS` under p99 and with the raster workers loaded

`jit_internal.h:305-316` sizes the branch LUT at 64 K entries = **512 KB per
CPU, 1 MB total**, sitting at the front of the arena (`LUT_AREA`,
`stubs.cpp:117`). The comment records that 1024 entries cost 1.6-2.7 % and 8 K
is break-even, and that bigger than 64 K gains nothing. On an RK3566 the whole
L3 is 512 KB and three raster workers stream through it; 8 K entries (64 KB)
was recorded as *break-even on the mean*, and the standing rule in the memory
notes is that means hid the dips. Worth one A/B at `LUT_BITS = 13` reporting
p99 and over-budget frames on GSDD and etody. Cheap (one constant, the arena
already reserves `LUT_BITS_MAX`). Low confidence; listed for completeness.

---

## Bugs and waste (separate list)

**B1 — dead code in `branch_indirect_cdi`, ARM9, Thumb-even path.**
`stubs.cpp:376-378`:
```cpp
e.bind(thumb);
size_t odd = e.tbnz_fwd(3, 1);
emit_fetch_cost9(e, 3, 4, 6, true);   // computes the cost into w4 ...
e.movz(4, 0);                          // ... and immediately discards it
```
`emit_fetch_cost9` with `branch=true` emits 6 instructions including a
timing-table `ldrb`, and `movz w4,#0` overwrites its only output
unconditionally. The comment two lines up ("even a: new r15 = a + 4 has bit 1
set -> numC 0") shows the zero is intended, so the call is simply leftover.
Delete it: 6 instructions and a load off every `POP {..,pc}`/`LDM {..,pc}` that
lands on a halfword-even Thumb target. Semantically a no-op — safe, and the
frame hashes must not move.

**B2 — dead instruction in the same stub.** `stubs.cpp:386-387`:
```cpp
e.add_imm(5, 4, 0);      // w5 = w4
e.add_reg(5, 4, 1);      // w5 = w4 + w1   (overwrites it)
```
One wasted instruction on the ARM9 `LDM pc` cost path.

**B3 — `emit_fetch_cost9`'s non-branch form keeps a data-dependent branch in a
hot stub.** `stubs.cpp:82-92` uses `b_cond_fwd(NE)` around the cacheable-page
case. The `branch_indirect` body was deliberately rewritten branch-free for
exactly this reason (see the comment at `stubs.cpp:444-453`: "The old form took
a data-dependent branch per state and one inside every S fetch (the 0xFF
cache-line test)"), but `branch_indirect_cdi` still calls the branching helper
three times. Same fix as item 2 supersedes it; until then it is a
mispredictable branch per `LDM pc`.

**B4 — global counter increments on every slow access.** Each of
`jit_h_ld8/16/32` and `jit_h_st8/16/32` opens with
`g_rt.stats.slow_accesses++` (`runtime.cpp:812, 817, 823, 829, 837, 843`), an
unconditional read-modify-write of a global on ~4.6 k accesses/frame. Trivial
in isolation, but it dirties a shared `Runtime` line on a path already inside
`call_pure`. Gate it behind `prof::enabled` or the existing stats knob if the
counter is only used by `report()`.

**B5 — not a bug, a documentation defect that misleads future work.**
`translate.cpp:11-18` states the block layout puts cold code where "the I-cache
holds only what runs". As shown in item 1 that is false at the arena level.
Whether or not item 1 is implemented, the comment should say what the code
actually does, because it is the kind of claim that stops someone re-examining
the layout.

**B6 — fidelity-vs-speed knobs that are correctly flagged.** `--cpu-oc`
(`jit.h:99-102`, `Translator::oc_data_cost`, `translate.cpp:496-500`),
`DS_JIT_FASTCOST` and `DS_JIT_COSTPROBE`/`MEMPROBE` are all inexact-by-design
and all documented as such at their definitions; `set_cpu_oc`/`set_strict`
flush both CPUs when they change (`jit.h:102, 109`). I found no *undeclared*
fidelity shortcut in the emitted fast paths. The one place worth a second look
is `emit_single`'s `const_nd` arm, where the charge deliberately lands *before*
the access rather than after (`translate.cpp:900-909`); the comment argues
nothing observes the budget in between and that strict mode checks per
instruction, which is correct for the current block shape but would silently
break if a budget check were ever emitted mid-instruction.

---

## Things I checked and am NOT proposing

* **Block dispatch/hash/LUT probe** — 7-8 instructions, no hash, one load, one
  tag compare (`stubs.cpp:292-303`); reached 4.5 k times/frame. Not the lever.
* **Block entry/exit prologue** — one `tbnz w8, #31` at entry
  (`translate.cpp:513-520`), one `sub` + one patched `b` at exit. Already
  minimal.
* **Conditional execution** — the `csel` form (`translate.cpp:1412-1426`) and
  the precharge trick (`arm_simple_cost`, `translate.cpp:1367-1375`) are both
  in place and well reasoned.
* **ARM9 data-cost combine** — `emit_charge_data_body` already specialises on
  the translate-time `nc` (`translate.cpp:847-862`), and for the common
  cacheable non-line-aligned fetch `nc == 1`, so the charge really is a single
  `sub`. Nothing to win here; the win is in the *lookup* (item 4).
* **ARM7 data cost** — already a precomputed table lookup (`cost7_slot`), and
  it is the model item 2 should copy for the ARM9 refill.
* **Load scheduling around the page walk** — `emit_single`
  (`translate.cpp:928-940`) already interleaves the timing load and address
  arithmetic between the entry load and its use, and the charge between the
  data load and its consumer, exactly as an in-order core wants. Confirmed by
  reading the emission order; do not "fix" it.
* **Register cache spills** — there are none in block bodies: all 15 guest
  registers are pinned (`convention.h:37-40`) and only the stubs spill.
* **Thumb handling** — same translator, `step()`-parameterised; no separate
  slow path.
* **Cold paths inlined in hot code** — none found; every rare path goes through
  `cold_begin`/`cold_end`. The problem is *where* the cold section lands
  (item 1), not that it is inline.
* **getenv/atomics in emitted code** — all `rt().*` knobs are read at translate
  time only. Clean.
* **Interpreter hot loop** — not a shipping path (see Facts).

----------------------------------------------------------------
# FULL REPORT: system

# System glue audit — scheduler / bus / page table / DMA / I/O / profile

Target: RG DS (4x A55, in-order, 32 KB L1D, 64 B lines).
Everything below is read from the code; disassembly quotes come from
`build/fl-o3plain/.../io.cpp.o` (the only non-LTO aarch64 object in the tree —
the normal builds are LTO bitcode and cannot be objdumped).

General verdict: the design is sound and most of the per-instruction paths are
already tight (page-table entry with tags folded into the pointer, delta-free
bitmask event table, DMA closed-form bulk runs, ARM7 cost table). The cost that
is left in this subsystem is **not** per-instruction; it is (a) MMIO register
dispatch, and (b) a small number of *control-register writes* that each drag a
few hundred KB of page-table/host-pointer traffic and a worker join behind them.

---

## Ranked opportunities

### 1. I/O register dispatch is a 15–20 deep compare tree per MMIO access
`src/core/io/io.cpp:937` (`Io::read`), `:1019` (`Io::read16`), `:949`/`:1066`
(write side).

What it does now: `Io::read` does the `io_unowned` range test plus up to three
`owns_reg` probes, then calls `read16`, whose body is `switch (addr)` over ~30
full 32-bit addresses spanning `0x04000004 … 0x04100010` plus a `0x04800000`
wifi range. GCC cannot build a jump table over that span, so it emits a binary
search. From the disassembly of `Io::read16`:

```
46a4: mov w2, #0x47fffff ; ccmp w19,w2 ; b.ls        <- wifi range test
46e4: cmp w19, w2(0x40001a2) ; b.eq ; b.hi           <- tree root
46f4: cmp w19, w5 ; b.hi
4704: cmp w19, w4 ; b.hi
4710: cmp w19, w0 ; b.hi   ... (55 conditional branches in the function)
```
`Io::read` itself adds ~8 more compares/branches before the call, and it is a
**non-tail call** with a 48-byte frame (`stp x29,x30,[sp,#-48]!`). So a VCOUNT
or DISPSTAT read — the hottest MMIO reads on the machine — costs roughly 15–20
data-dependent branches, most of them cold in the BTB because the address
distribution is bimodal per game. On an in-order A55 at ~8 cycles per
mispredict this is tens of cycles of pure dispatch on top of the JIT's slow
helper call. A 32-bit read of a non-`read32_special` register walks the tree
**twice** (`read16(addr) | read16(addr+2)`, io.cpp:944).

Proposal (exact, frame-hash preserving): peel the three non-`0x04000xxx`
windows (`0x0410xxxx`, `0x048xxxxx`, and the ≥`0x04001000` tail) out first,
then switch on `reg = addr & 0xFFF` — dense 0…0xFFF, which GCC turns into one
jump table (`ldrh`+`br`), i.e. one indirect branch instead of seven compares.
Better still, a 1024-entry `u8` handler-id table indexed by `reg >> 2` built at
startup, with the 8/16/32 split resolved inside the handler; that also removes
the byte-write read-modify-write path (`io.cpp:1183`) for the registers that
care. Keep `read32_special`/`write32_special` as they are — they already
short-circuit the hottest 32-bit ports.

Win: honest estimate 0.2–0.8 ms/frame on I/O-heavy scenes. Measure with
`DS_IO_CENSUS=1` first (it prints the per-address histogram — that says exactly
how many accesses a frame you are shortening), then dbori (25 k GXSTAT polls a
frame, per memory) and GSDD; qemu hotblocks for the sub-1 % confirmation, since
this is squarely inside the A/B noise band on device.
Risk: low-medium — mechanical but touches every register; the frame-hash oracle
on all five scenes covers it. Effort: a day.

### 2. Every VRAMCNT *byte* write does a full VRAM re-map, a render catch-up and a worker join
`src/core/io/io.cpp:1168` → `src/core/mem/bus.cpp:144` (`Bus::update_vram`) →
`src/core/gpu/gpu.cpp:235` (`vram_remap_begin`).

```
io.cpp:1168  if (vramcnt[bank] != value) { vramcnt[bank] = value; nds_.bus.update_vram(); }
bus.cpp:149  nds_.gpu3d.sync_raster();
gpu.cpp:236  catch_up(3); join_worker(); ... disarm_trap();
bus.cpp:175  std::memset(h9, 0, VRAM_PAGES * sizeof(u8*));   // 8192 * 8 = 64 KB
bus.cpp:176  std::memset(h7, 0, VRAM_PAGES * sizeof(u8*));   // another 64 KB
bus.cpp:196  pt9.remap(0x06000000, 0x01000000, h9, RW);      // 8192 entries read+compare
bus.cpp:197  pt7.remap(...);                                 // 8192 more
bus.cpp:198  nds_.gpu.vram_remap_end(trapped);               // arm_trap: 4096-8192 more
```
So one changed VRAMCNT byte costs ≈128 KB of memset, ≈128 KB of page-table
read-modify-write, another 32–64 KB for the trap re-arm, a 3D raster
`sync_raster()` and a 2D `catch_up(3)` + `join_worker()`. And `write16` splits
into two `write8` calls (io.cpp:1138), so a 32-bit store to `0x04000240` — the
normal way a game programs banks A–D — pays that **four times**: ~1 MB of
memory traffic and up to four worker joins for one guest instruction. That is
far more than the 32 KB L1D and blows the JIT's page-table working set out of
L1/L2 as a side effect.

Proposal: defer. This is exactly what `docs/techniques/04` §5 describes DraStic
doing ("a `VRAMCNT` write sets a bit in `page_dirty[]`; `video_render_scanlines`
applies the pending remaps at the top of its next run"). Concretely: mark a
`vram_dirty_mask` bit per bank in the store handler and apply one coalesced
`update_vram()` at the next point that can observe it — the next 2D render
range, the next CPU/DMA access that falls through to the VRAM slow path, and
the frame boundary. The join/catch-up then happens once per group instead of
once per byte.
Exactness: bit-exact if the flush points cover every observer (renderer, CPU
loads, DMA, 3D texture views); the risk is missing one, so gate it behind a
knob and diff frame hashes on all five scenes.
Win: large on capture/bank-swapping titles (GSDD swaps capture banks every
frame; the memory notes VRAMCNT traffic as routine). Measure with
`DS_DEBUG_VRAMCNT=1` for the call count, then GSDD/etody p99 — this is a *tail*
item as much as a mean item, because the join is what makes it spiky.
Risk: medium. Effort: 2–3 days including the exactness proof.

### 3. `set_write_trap` walks 4096–8192 page-table entries per arm/disarm
`src/core/mem/page_table.cpp:143`, called from `Bus::set_vram_trap`
(`bus.cpp:356`) via `Gpu::arm_trap`/`disarm_trap` (`gpu.cpp:245`, `:264`).

```
for (u32 p = first; p < first + count; ++p) {
  const Entry e = table_[p];
  if (!(e & BASE_MASK)) continue;
  table_[p] = on ? (e | TAG_SPECIAL) : (e & ~TAG_SPECIAL);
}
```
`set_vram_trap(on, lcdc)` covers 8 MB (+8 MB LCDC) = 4096 (+4096) entries,
i.e. 32–64 KB streamed through L1D per toggle. The comment at `gpu.cpp:250`
already records that Golden Sun toggles ~31 times a frame — that is ~250 k
entry visits and ~2 MB of traffic per frame just to move a tag bit. Two cheap
fixes, both bit-exact:
 - **Iterate only the mapped pages.** Of the 8192 entries in `0x06000000+`,
   only ~330 have a base (656 KB of VRAM at 2 KB/page); the rest are `continue`d
   after being loaded. `update_vram` already builds the host-pointer array, so
   it can record the list (or a bitmap) of mapped VRAM page indices; the trap
   then touches ~330 entries instead of 8192. ~25x less traffic.
 - **Don't store when the bit is already right** (`if (((e >> 62) & 1) == on) continue;`)
   — avoids dirtying clean lines and their writeback.
Win: order 1 % on trap-toggling titles, more on the tail; the earlier
"arm only the batching engine's windows" experiment lost (+0.9 %) precisely
because it *added* toggles — this removes cost from every toggle instead, so it
is the complementary change and should not repeat that result.
Measure: GSDD and Golden Sun-shaped scenes, p99. Risk: low. Effort: half a day.

### 4. `prof::enabled` is tested inside `PageTable::write_ptr` — on every store
`src/core/mem/page_table.h:128`:
```
inline u8* write_ptr(u32 addr, bool* is_code) const {
  if (prof::enabled) {          // census: what a write-path dirty bit must intercept
    const u32 r = addr >> 24;
    if (r == 5) prof::add(prof::C_W_PALETTE, 1);
    else if (r == 7) prof::add(prof::C_W_OAM, 1);
  }
  Entry e = table_[addr >> PAGE_SHIFT];
  ...
```
`prof::enabled` is a mutable extern global, so LTO cannot fold it: every
interpreter store, every JIT slow-path store helper and **every DMA per-unit
store** (`bus.cpp:398-412`) pays a global load + branch, and the census body
inflates the inlined function so it is less likely to be inlined at all. The
brief's rule is that disabled instrumentation must cost zero; this one does not.
Proposal: put the palette/OAM census behind a compile-time `DSPERATE_PROFILE`
macro (or move it into the two call sites that actually want it), leaving
`write_ptr` as pure table lookup + two bit tests. Same for
`prof::add(prof::C_DMA_LOOP, 1)` at `dma.cpp:257`, which sits in the DMA
per-unit loop.
Exact. Win: small but free (a load+branch per store; stores are ~15–20 % of
guest instructions). Measure: qemu hotblocks on sm64/dbori. Risk: none.

### 5. `ipc_sync_write` yields the slice unconditionally
`src/core/io/io.cpp:121`:
```
  // The other CPU may be waiting on this with a tight timeout ...
  nds_.sched.yield(nds_.cpu(cpu));
```
`Scheduler::yield` (`scheduler.h:87`) throws away the rest of the ARM9's slice
and hands the machine to the ARM7 — a full slice boundary: `fire_due`,
`machine_idle`, `gpu3d.run_to`, the ARM7 budget arithmetic, and on the JIT path
a return through `slice_next` and re-entry via `jit::lookup`. It fires on
*every* IPCSYNC write, not only the boot handshake the comment describes. SDK
IPC code writes IPCSYNC in ordinary game loops.
Proposal: yield only when the write can matter — when the *output nibble
actually changes* and the other CPU is awake (not halted), or keep a small
"handshake window" (first N frames / until the game's main loop starts). Both
are behaviour-affecting (they change interleave), so this is **not** bit-exact
and must be re-validated against the titles listed in the comment (Platinum,
Zelda ST, DQIX) — those are the reason it exists.
Win: measure first. `DS_PROFILE=1` prints `slices`; compare slice count per
frame with the yield suppressed on mlbis/Platinum. If the slice count drops
>10 % there is real money here; if not, drop the idea.
Risk: high (this is the class of change that hangs games at boot). Effort: low
to measure, and the measurement is the whole point.

### 6. `update_tcm` rebuilds WRAM *and* VRAM on any TCM window change
`src/core/mem/bus.cpp:249-273`:
```
  update_wram();
  update_vram();     // -> gpu3d.sync_raster(), gpu.catch_up(3), join_worker(), 128 KB of memsets
```
A CP15 DTCM/ITCM move has nothing to do with the VRAM banks or the shared-WRAM
split; they are re-applied only because the function starts by clearing the
whole low 256 MB (`pt.unmap(0, 0x10000000)` on `force`) or the previous TCM
windows. In the non-forced path the code has already narrowed the unmap to the
previous windows (`bus.cpp:264-265`), so `update_wram()`/`update_vram()` are
only needed when the *new or old* TCM window overlaps `0x03xxxxxx` /
`0x06xxxxxx` — which for a DTCM at the usual `0x027Cxxxx`/`0x0B000000` it never
does. Guard both calls on window overlap.
Exact (the mappings are identical; only the work is skipped). Win: removes a
raster join and ~256 KB of traffic from every TCM reprogram — cheap insurance
for titles that move DTCM per task switch. Measure: count `update_tcm` calls
with a temporary counter; if a scene calls it more than a handful of times a
frame it is worth it, otherwise note and move on. Risk: low. Effort: an hour.

### 7. DMA fixed-source (fill) and fixed-destination transfers never reach the bulk path
`src/core/dma/dma.cpp:325` and `:409` both gate the run on
`c.src_inc == 1 && c.dst_inc == 1`. Everything else goes one unit at a time
through `bus.dma_write32(bus.dma_read32(...))` (`dma.cpp:401`, `:465`), i.e. two
page-table walks plus a `unit_cycles()` table walk per word.
The important missing shape is **`src_inc == 0`** — the classic DMA fill (clear
VRAM/OAM/main RAM from a fixed source or from DMAxFILL). Its cost model is
already closed-form (`run_cost` picks the WRITE burst table when the
destination is main RAM, or a constant otherwise), so the run can be a
`memset`-style splat of the one source word with exactly the same cycle
accounting the per-unit loop produces. Fixed-*destination* runs other than the
GXFIFO one (`dma.cpp:263`) are a second, smaller case.
Exact by construction (same bytes, same closed-form cost, same budget-cut
binary search as the existing runs). Win: proportional to
`C_DMA_SLOW_W`/`C_DMA_SLOW_H` in `DS_PROFILE=1` output — read those counters on
GSDD, sm64 and mlbis before writing any code; if they are small, skip it.
Risk: low (it is a fourth copy of a pattern that already exists twice). Effort:
half a day, mostly test.

### 8. `CpuContext` puts the memory-path fields ~250 bytes away from `JitHot`
`src/core/cpu/cpu.h:37-95`. Order is: `which/halted/preempt_residual` (16 B),
`JitHot` (104 B), then ~200 bytes of *cold* state (banked registers, all of
CP15, `pu_region[8]`), and only then the fields every single guest memory access
touches: `timing9`, `timing7`, `cost7`, `code_cycles`, `data_cycles`,
`code_region`, `data_region`, and `page_table` (whose `table_` pointer is the
base of the walk). That is at least three separate 64 B lines for what is one
logical hot set, and the interpreter's `data_cost` (`cpu/cpu_mem.h:22`) touches
`timing9` + `data_cycles` + `data_region` on *every* load and store.
Proposal: move `timing9/timing7/cost7/code_cycles/data_cycles/code_region/
data_region` and the `PageTable` object immediately after `JitHot`, pushing the
banked registers and CP15 to the tail. The JIT's `static_assert`s are on
`JitHot`'s internal layout and its offset is unchanged if the block is inserted
after it — but every hard-coded offset in `jit_internal.h`/the emitters must be
re-derived, so do this with the offsets asserted.
Exact. Win: 1–2 fewer L1D lines in the CPU-emulation working set; on an
in-order core with a 32 KB L1D shared with a 16 MB page table this is the kind
of thing that shows up as p99 rather than mean. Measure: PMU L1D refills on
.20, sm64 + GSDD. Risk: medium (JIT offset contract). Effort: a day.

### 9. JIT slow-access helpers do an unconditional global RMW
`src/core/cpu/jit/runtime.cpp:800, 807, 814, 821, 831, 840`:
`g_rt.stats.slow_accesses++` on every MMIO load and store from translated code.
A read-modify-write of a global on the hottest fallback path, in the same
cacheline neighbourhood as whatever else lives in `g_rt.stats` (check it is not
shared with anything a worker touches — if it is, that is false sharing on the
MMIO path). Gate it on the same compile-time profile switch as item 4.
Exact, trivial, small.

### 10. `Io::read`/`write` for 32-bit accesses to non-special registers walk the tree twice
`io.cpp:944`, `:960`: `read16(addr) | (read16(addr+2) << 16)` and the mirrored
write. With the item-1 dispatch table this disappears naturally (one lookup,
two handler calls at most); without it, the second walk is ~7 more branches.
Fold into item 1 rather than doing separately.

---

## Bugs and fidelity-vs-speed notes

**B1. DMA save-state loses the WRITE burst tables** — `dma.cpp:519`:
```
u8 bt = c.burst_table == READ16.data ? 1 : c.burst_table == READ32.data ? 2
      : c.burst_table == READ32_N2.data ? 3 : 0;
```
`WRITE16`, `WRITE32` and `WRITE32_N2` (all three are live tables, selected at
`dma.cpp:161-165`) map to `0` and are restored as `MRAM_DUMMY`. A state saved
mid-transfer on a main-RAM-destination channel comes back with the wrong burst
pattern, i.e. wrong DMA timing after a load. Needs three more indices and a
format-version bump. (Save states are taken at a frame boundary where no
channel should be mid-run, which is probably why it has not bitten — but
`in_progress` channels do survive, so it is reachable.)

**B2. `Scheduler::run_until_impl` and `Scheduler::slice_next` are two hand-kept
copies of the same state machine** (`scheduler.cpp:329-426` vs `:453-508`). The
comment at `:320` acknowledges it and says the strict slice diff checks them.
Any change to slice policy (items 5, and the idle-skip logic) must be made
twice, and only the JIT copy is exercised on device. Not a bug today; it is the
highest-risk maintenance surface in this subsystem.

**B3. `Bus::io_write` GXFIFO bypass depends on a cross-TU predicate**
(`bus.cpp:338`, `io::Io::census_on()`). Under LTO it folds to a global load;
in a non-LTO build (the ARM32 tier, `fl-o3plain`) it is an out-of-line call on
the hottest 3D store path. Make it an inlined `extern bool`.

**B4. Fidelity note, already known but visible here:** `Scheduler::now()`
(`scheduler.h:68`) is exact inside a slice, but events still only *fire* at
slice ends (`fire_due` at `scheduler.cpp:423`/`:505`). The DIV/SQRT/SPI busy
bits were moved to time comparisons precisely to work around this
(`io.h:107-118`); the remaining periodic handlers correctly reschedule from
`event_time()` rather than `now()`, which is right. No action, but any new
"busy" bit should follow the DIV/SQRT pattern rather than an event.

**B5. `Io::write8` generic path is a read-modify-write through `read16` +
`write16`** (`io.cpp:1181-1184`) — two full dispatch-tree walks for one byte
store, and it also means a byte write to a write-only register reads it first.
Correctness is fine for the registers that reach it; it is another reason to do
item 1.

---

## Things I checked that are already right (do not spend time here)

- Event table: split-by-field arrays gated by a 20-bit `armed_` mask with a
  cached `next_`/`next_id_` and a `ctz` walk (`scheduler.cpp:59-91`, `:256-282`).
  Insertion is O(1) in the common case, firing touches only armed entries.
  Nothing to gain from a heap or a delta list at this size.
- `run_cpu`'s only per-slice test is the sign of `hot.cycle_budget`; the
  interleave itself is a handful of well-predicted branches, and the JIT path
  is a goto-threaded state machine deliberately avoiding indirect dispatch
  (`scheduler.cpp:329`).
- Page-table entry design (pre-biased base with the two tags in the top bits,
  `e << 2` recovering the pointer and discarding both tags) — a load from a
  code page is genuinely as cheap as one from plain RAM. The JIT's emitted walk
  (`a64/translate.cpp:758-768`) is 4 instructions for a load, 6 for a store.
- DMA closed-form burst costing (`RunCost::bulk`, `dma.cpp:189-203`) with the
  "test the whole run first, binary-search only if the budget cuts it"
  ordering — this is well done.
- `state/state.h` is templated Writer/Reader over `std::vector<u8>`; nothing in
  it is reachable from the run loop, and `save_state` refuses unless
  `sched.at_slice_boundary()`. Clean.
- `input/input_log.cpp` is per-frame record/replay only.
- No atomics, mutexes, `std::function` or virtual calls anywhere in the
  per-instruction paths of this subsystem; the only atomic is `Timing::stamp`,
  written once per retime and read by the pre-translation worker (a correct
  seqlock, not a hot-path cost).

----------------------------------------------------------------
# FULL REPORT: r3d

# 3D pipeline audit (gpu3d, render3d, kernels, texcache)

Scope: `src/core/gpu/{gpu3d,render3d,kernels_neon,texcache}.*`, `line_worker.h`.
All line numbers are from the working tree as read on 2026-09-07. Nothing was edited.

The renderer is already close to the DraStic shape the docs describe: per-polygon
kernel selection (`Shade::resolve`, `Shade::gather4`), a batched span pipeline,
byte-plane resolve/final-pass kernels, constant-attribute fast paths. The items
below are what is left, ranked by expected value.

---

## 1. Every band worker rebuilds the whole edge/Shade array for every polygon (~490 B/polygon x 3 workers)

**Now.** `render3d.cpp:3013 build_edges()` walks the *entire* polygon list and calls
`setup_polygon()` on each survivor, and it runs once per `Renderer3D` instance:

```
for (u32 i = 0; i < gx.render_polygon_count(); ++i) {
  if (polys[i]->degenerate) continue;
  setup_poly_ = i;
  setup_polygon(edges_[n++], *polys[i]);        // render3d.cpp:3019
}
```

`prepare_worker` (3010) calls it for workers 1..n-1 and job 0 calls it for itself
(`render3d.cpp:2643`). `setup_polygon` → `setup_shade` (1025) → `texture_fields`
(985, with the `direct_range` block-walk loop), the per-vertex uniformity loop over
`p.nverts`, `select_gather4`, `select_resolve`, then `setup_left_edge`/
`setup_right_edge` (each a `Slope::setup` with two divisions) and
`refresh_edge_state`.

`Edge` (render3d.h:216-243) is two `Slope`s (each embedding a 56-byte `Interp`), a
whole `Shade` (~160 B, including a 16-byte pointer-to-member `resolve`), four vertex
pointers and twelve cached s32 — roughly **490 bytes**, `std::array<Edge, 2048>`
= ~1 MB per instance. On GSDD's title (1301 live polygons) that is ~640 KB of
stores per worker per frame, ~1.9 MB per frame in total, and it evicts the 25 KB
of colour/depth/attr ring the whole tile scheme depends on staying in L1/L2.
On an in-order A55 this is pure store-buffer and L2 traffic before a single pixel
is drawn.

Worse, most of it is dead work: with three bands each worker only draws ~1/3 of the
scanlines, and most polygons are small enough to touch exactly one band.

**Change.** Make edge/Shade construction lazy per instance. Keep the counting sort
by `ytop` (which only needs `Polygon::ytop`, no `Edge`), and build `edges_[i]` on
first use — at the point `render_chunk` merges a polygon into the active set
(`render3d.cpp:2075-2110`) and at `seed_active` (3038), with a per-instance
"built" bitmap cleared per frame. A cheaper half-measure with no bookkeeping: in
`build_edges`, skip polygons whose `[ytop, ybot)` cannot intersect any line this
instance will draw — but bins are claimed dynamically, so this needs the bin range
to be passed in, which is a smaller change than laziness but only helps when
bins == workers.

**Exactness.** Bit-exact: `Slope::setup` positions an edge from `y` directly and
`rewind_edge` already exists for re-entry, so nothing depends on when the record
was built.

**Size.** Directly proportional to polygon count: GSDD/dbori are the scenes.
Expect 1.5-4 % of the raster on GSDD, less on sm64. Measure with qemu hotblocks
(instruction counts in `setup_polygon`/`setup_shade`/`Slope::setup`) plus a device
A/B on GSDD and dbori; watch `C_BAND_SUM_NS`.

**Risk/effort.** Medium. The trap is `rendered_upto_`/`rewind_edge` interacting
with lazy construction — a lazily built edge is by definition at `ytop`, so the
rewind logic must not also rewind it.

---

## 2. `mul_hi8_add` widens to 64 bits for interpolants that fit in 32 (and colour fits in 16)

**Now.** `kernels_neon.cpp:907`:

```
inline int32x4_t mul_hi8_add(int32x4_t base, uint32x4_t d, uint32x4_t f) {
  const uint64x2_t lo = vshrq_n_u64(vmull_u32(vget_low_u32(d), vget_low_u32(f)), 8);
  const uint64x2_t hi = vshrq_n_u64(compat::mull_high_u32(d, f), 8);
  return vaddq_s32(base, vreinterpretq_s32_u32(vcombine_u32(vmovn_u64(lo), vmovn_u64(hi))));
}
```

This is the inner operation of `span_attrs5n` (1059), `span_attrs2n` (1035) and
`span_attr_persp` (994) — the main attribute interpolation kernel, run over every
staged pixel of every perspective span. `vmull_u32` produces **two** lanes per
instruction and each 128-bit op issues over two cycles on the A55, so four pixels
of one attribute cost 2 mull + 2 shift + 2 movn + 1 combine + 1 add ≈ 8 vector
ops; five attributes x 8 pixels ≈ 80 ops per 8 pixels.

The 64-bit width is not needed. The colour endpoints are `Vertex::fcol`, 9 bits
(`gpu3d.cpp:1063 final_colour`: `(c << 4) + 0xF` ≤ 0x1FF), so `d ≤ 0x1FF`; texture
`d < 2^17`; the factor is a 8-fraction blend normally ≤ 256. So:

- s/t: `vmulq_u32(d, f)` + `vshrq_n_u32(...,8)` — 3 ops per 4 lanes instead of 8.
- r/g/b: `vmull_u16(d16, f16)` gives 4 u32 lanes from 16-bit inputs; `d ≤ 0x1FF`
  and `f ≤ 0x100` fit u16 and the product fits u32, so the whole colour chain can
  run at **8 lanes per pair of instructions** with `vshrn_n_u32(p, 8)` →
  u16, add base, `vshrn_n_u16(v, 3)` → the byte plane the shader wants. That
  removes both the widen-to-64 and the narrow-back churn the brief asks about.

**Guard.** `span_factor` (915) does not bound the factor: it only checks
`vcgeq_u32(q, big /* 0x3FFFFF */)` to decide whether the reciprocal path was
provable. Add a `vmaxq_u32` accumulator to `span_factor` and return (or store) the
maximum factor for the span; take the narrow path only when `max <= 0x1FF`, else
fall back to the current kernel. One extra vector op per four pixels in
`span_factor`, paid once against five attributes.

**Exactness.** Bit-exact under the guard (`(d*f) >> 8` is the same value computed
in a narrower type).

**Size.** `span_attrs5n`/`2n` is one of the two highest-count 3D kernels
(`C_SPAN_PIXELS`). Halving its vector op count should be worth 1-3 % of the raster
on etody (35 % band-wait — the scene the brief says can actually measure a raster
change) and dbori. Measure with `DS_PROFILE` `R3D_SPANS` on etody, cross-checked
with qemu hotblocks on the kernel symbol.

**Risk/effort.** Low-medium; `tests/kernels_test.cpp` already diffs ref vs neon
vector by vector, so the guard is testable directly.

---

## 3. `depth_candidates` processes four pixels per iteration and stores four bytes at a time

**Now.** `kernels_neon.cpp:1252 depth_candidates_m`: per iteration it loads
`z`, `dstz`, `dstattr` (three `vld1q`), does the compare, then

```
const uint16x4_t v16 = vmovn_u32(v);
const uint8x8_t v8 = vmovn_u16(vcombine_u16(v16, v16));
vst1_lane_u32(reinterpret_cast<u32*>(pass + i), vreinterpret_u32_u8(v8), 0);
```

— two narrowing moves, a redundant `vcombine` and a **4-byte lane store** for every
four pixels, plus `any = vorrq_u32(...)` per iteration. Then a scalar byte scan for
`first`/`last`.

This kernel runs over every staged pixel of every span (it is the depth pre-pass,
`render3d.cpp:1861`), so its per-4-pixel overhead is multiplied by `C_SPAN_PIXELS`.

**Change.** Unroll to 16 pixels: four compares → one `narrow(a,b,c,d)` (the helper
already exists at `kernels_neon.cpp:31`) → one `vst1q_u8`. Keep a 4-pixel tail.
Derive `first`/`last` from the 16-byte mask with two 64-bit lane extractions and
`clz/ctz` instead of the scalar byte scans.

**Exactness.** Bit-exact.

**Size.** ~0.5-1.5 % of the raster; larger on the occlusion-heavy scenes where the
pre-pass is most of what a span costs (`C_SPAN_OCCLUDED` is the census that sizes
it). Measure on etody and GSDD with qemu hotblocks.

**Risk/effort.** Low.

---

## 4. `Interp::set_x` divides once per scanline per edge even when the denominator is constant

**Now.** `render3d.cpp:54`:

```
if (xdiff != 0 && (!linear || wbuffer)) {
  const u32 num = static_cast<u32>(xv * w0n) << shift;
  const u32 den = static_cast<u32>(xv * w0d) + static_cast<u32>((xdiff - xv) * w1d);
  yfactor = den == 0 ? 0 : num / den;
}
```

`set_x` is called from `Slope::step()` (154) for both edges of every active polygon
on every scanline it covers. A 32-bit `udiv` on the A55 is ~12 cycles, not
pipelined, and the result feeds the fourteen `interpolate` calls in
`precompute_lines` — so it is on the critical dependency chain of the edge walk.

Two cases divide unnecessarily:
- `w0d == w1d` (W equal along the edge): `den = xdiff * w0d`, a **constant** for
  the whole edge run.
- `wbuffer && linear`: the condition `(!linear || wbuffer)` forces the division
  even though `linear` means `w0 == w1`, i.e. exactly the constant-den case.

**Change.** In `Interp::setup` (43), when `w0d == w1d`, precompute
`den_recip = ceil(2^32 / (xdiff * w0d))` and set a flag; `set_x` then computes
`yfactor` by the `mulhi` + one-compare fix-up already used for `recip` in
`Interp::interpolate` (76-79). Everything else keeps the divide.

**Exactness.** Bit-exact (the same fix-up pattern is already proven in this file).

**Size.** One `udiv` removed per edge per scanline on constant-W edges — DS content
is full of them (this is DraStic's `setup_perspective_steps_w_constant`). Order
0.5-1.5 % of the raster; sm64 and etody have the most polygon lines per pixel.
Measure with `C_POLY_LINES` x device A/B on etody, or qemu hotblocks on
`Slope::step`.

**Risk/effort.** Low.

---

## 5. Profiling flag reloaded inside the innermost resolve loop

**Now.** `render3d.cpp:1599` and `:1631`, inside `resolve_span_vec`'s per-8-pixel
`group` lambda:

```
prof::add(prof::C_RESOLVED_PIXELS, NH * 4);
...
const u64 kinds = lanes8(kv[0], kv[1]);
if (prof::enabled) rk_census(kinds, p8);
```

`prof::add` is `if (enabled) detail::get()->count[c] += n;` and `detail::get()`
can call the opaque `make_acc()`, so the compiler must reload the `enabled` global
on every group. That is two global loads and two branches per eight pixels in the
hottest loop in the renderer. The same file already knows this is a real cost —
`stage_line` (render3d.cpp:1836) hoists it deliberately: *"One load of the
profiling flag for the whole line: the kernels between the checks are opaque to
the compiler, so every test re-read it (~50 insn/line)."*

The scalar `resolve_span` is worse: `prof::add(prof::C_RESOLVED_PIXELS, 1)` at
line 949 is **per pixel** (shadow/wireframe polygons only).

**Change.** Read `prof::enabled` once in `resolve_batch_vec` (1948) into a local
and pass it as a template parameter or a captured `const bool`; accumulate the
pixel count in a local and flush it once per batch. Same for the scalar path.

**Exactness.** Bit-exact (counters only).

**Size.** 2-4 % of the resolve stage; the resolve is 20-40 % of the raster on
etody. Measure on etody with a device A/B and confirm with qemu hotblocks
(instruction count in `resolve_batch_vec`).

**Risk/effort.** Trivial.

---

## 6. Occluded head and tail of every span stay in the batch and are shaded

**Now.** `stage_line` (render3d.cpp:1826) stages the full `[xa, xb)` span into the
batch and advances the cursor by the full width:

```
batch_px_ += static_cast<u32>(xb - xa);      // render3d.cpp:1884
```

but the depth pre-pass narrows the live range to `[ca, cb)` (1861-1864) and
`span_attrs` only fills `[ca, cb)`. `flush_batch` (1980) then runs `span_texels`
and `span_shade` over `[0, batch_px_)` — i.e. over the occluded prefix
`[xa, ca)` and suffix `[cb, xb)` as well, gathering texels for stale s/t and
shading pixels whose `pass` byte is zero.

**Change.** At minimum `batch_px_ += (cb - xa)` reclaims the occluded *suffix* with
no other change (the staged region is `[off, off + cb - xa)` and nothing reads past
`cb`). Reclaiming the prefix needs the depth stage to write `z` at a scratch offset
and the attributes to be staged compacted at `off` — a bigger change, worth it only
if the census says the prefix is large.

**Exactness.** Bit-exact.

**Size.** Size it first: compare `C_SPAN_PIXELS` (staged) with `C_RES_VEC_PX` +
`C_RES_TOON_PX` (drawn) in a `DS_PROFILE` run on GSDD and etody — the ratio is the
wasted fraction of the texture and shade stages.

**Risk/effort.** Low for the suffix; medium for the prefix.

---

## 7. Untextured polygons never batch

**Now.** `render3d.cpp:2129`, inside the per-line loop of `render_chunk`:

```
// Batching only pays where the pixel stages are worth amortising. An
// untextured polygon's shading is a handful of NEON ops over a dozen
// pixels, so those flush immediately.
if (!e.sh.textured) flush_batch(e.sh);
```

So an untextured polygon pays a full `flush_batch` — including the indirect
`(this->*sh.resolve)` call and the resolve kernel's prologue — **per scanline**,
and `span_shade` rounds `n` up to 16 (`render3d.cpp:1376`) per line: a 5-pixel
untextured span shades 16 lanes, a 3x waste that batching would remove because
`stage_line` packs spans contiguously in the buffer.

**Change.** Re-measure with untextured polygons batched like textured ones (drop
the line, keep the `batch_px_ >= BATCH_PX` flush). The counter-argument in the
comment predates the byte-plane `span_shade` and the batch-level resolve.

**Exactness.** Bit-exact (batching across spans of one polygon is already argued
safe at `render3d.cpp:1975-1979`).

**Size.** Unknown; the census to look at is `C_BATCH_SPANS`/`C_BATCHES` and the
short-span histogram `C_SL0..C_SL2`. sm64 and mlbis have the most untextured
geometry. Honest expectation: ±1 %, hence the low rank.

**Risk/effort.** Trivial to test, and if it loses, the comment should record the
new measurement rather than the old reasoning.

---

## 8. Texture cache lookup is a hash-map probe per polygon with no memo

**Now.** `render3d.cpp:2578-2587`, once per polygon on the emulation thread:

```
Shade sh;
if (texture_fields(sh, p))
  poly_texels_[i] = texcache_.lookup(*vm_, sh.fmt, sh.base, ..., sh.texpal, sh.alpha0);
```

`TextureCache::lookup` (texcache.cpp:149) builds a 64-bit key and does
`entries_.find(k)` on an `std::unordered_map` — a hash, a bucket load and a node
pointer chase, per polygon, on the thread the whole frame waits on.
docs/techniques/02 §5 records that DraStic memoises this against the previous
polygon's parameters because *"games submit geometry grouped by material, so the
one-entry memo hits nearly always"* — we do not have that memo.

**Change.** Cache `last_key_` / `last_entry_` in `TextureCache` and short-circuit
`lookup` when the key repeats (still stamping `used`/`validated`). Also note
`texture_fields` is being run here purely to derive the key, and then again per
polygon inside `setup_shade` on every band worker — item 1 removes the second
part; a memo removes most of the first.

**Exactness.** Bit-exact.

**Size.** Small in cycles (the docs measure DraStic's whole texture cache at
0.04 %), but it is on the *emulation* thread's critical path in `render()`, which
is where GSDD is bound (memory: "GSDD is main-thread-bound"). Measure with
`C_TEXCACHE_HIT` and the GSDD main-thread breakdown.

**Risk/effort.** Trivial.

---

## 9. `Slope::setup` computes AA state when AA is off

**Now.** `render3d.cpp:150`: `if (xmajor) xcov_incr = (ylen << 10) / xlen;` runs on
every edge setup, but `xcov_incr` is only read by `edge_params` under `if (aa)`
(174-186), and the SDL frontend makes AA opt-in (`video.aa`, render3d.h:76-79),
so on the shipping default this division is always dead. Edge setups happen per
polygon per vertex segment per band worker.

**Change.** Pass the frame's AA bit into `Slope::setup` (it is already in
`dispcnt_`) and skip the division; `Interp::setup`'s `xrecip_z` (46) is likewise
only read by the non-W-buffer branch of `interpolate_z` and could be conditioned on
`!wbuf`.

**Exactness.** Bit-exact.

**Size.** ~10 k divisions/frame on GSDD at ~12 cycles ≈ 0.07 ms — call it 0.3 %.
Bundle with item 1, which touches the same call path.

**Risk/effort.** Low.

---

## 10. Final pass reads four neighbours with `ld4` to fetch one byte plane

**Now.** `render3d.cpp:2274-2286` (edge marking), the `nb()` lambda:

```
auto nb = [&](u32 naddr) -> uint8x16_t {
  const uint8x16_t nid = vld4q_u8(ab + naddr * 4).val[3];
  ...
```

`vld4q_u8` deinterleaves 64 bytes into four registers — on the A55 that is a
multi-uop structured load — and three of the four planes are thrown away. It runs
four times per 16 pixels (left, right, up, down) purely to obtain the polygon-id
byte, plus four `vld1q_u32` for the depths.

**Change.** Read the neighbour ids as `vld1q_u32` x4 + `vshrq_n_u32(a, 24)` and do
the compare in 32-bit lanes alongside the depth compare that is already there,
narrowing once. That drops four `ld4`s per 16 pixels for four `ld1q` groups you
already need for the depths.

**Exactness.** Bit-exact (`selftest_final_pass`, render3d.cpp:2416, checks the
whole pass against `final_pass_ref`).

**Size.** Only pays when edge marking is on (DISPCNT bit 5), which is title- and
scene-dependent — check which of the five scenes actually enables it before
spending time here. Ranked low for that reason.

**Risk/effort.** Low, and there is a self-test.

---

## 11. `std::function` heap allocation per rendered frame

**Now.** `render3d.cpp:2638`:

```
job_fn_ = [this, &gxr, dst](u32 w) { ... };
```

The closure captures three pointers = 24 bytes, over libstdc++'s 16-byte
`std::function` small-object buffer, so this is a `malloc` + `free` on the
emulation thread for every rendered frame, plus an indirect call per worker.

**Change.** Store the three values in members and make the job a plain
`void (*)(Renderer3D*, u32)`, or shrink the capture to `this` alone (`gxr` and
`dst` are already derivable from members: `gx_` and `out_[display_ ^ 1]`).

**Exactness.** Bit-exact.

**Size.** ~100 ns/frame — negligible in the mean, but it is an allocator call on
the thread that owns the frame deadline, so it can show in p99. Measure only as
part of a larger main-thread pass.

**Risk/effort.** Trivial.

---

## Bugs / fidelity-vs-speed notes

1. **`span_factor`'s constant-W ramp is knowingly inexact.** `kernels_neon.cpp:938`:
   *"The step is held in 16.16, so the ramp accumulates a rounded step and drifts
   from the exact quotient by up to a couple of units over a long span. That is a
   deliberate departure from bit-exactness."* This is documented and intentional,
   but it means the NEON build is **not** frame-hash comparable with the reference
   build on any constant-W perspective span, and any future "exactness" claim about
   the span stages has to except this. It also silently widens: the drift grows
   with span length, so a 256-pixel span is the worst case. Worth a `DS_R3D_EXACT`
   switch that takes the division path, if only so divergence hunts have a lever.

2. **`depth_candidates`'s `first` scan is unbounded in principle.**
   `kernels_neon.cpp:1276`: `u32 first = 0; while (!pass[first]) ++first;` is
   guarded only by `maxv_u32(any) != 0`, and `any` includes lanes in the tail past
   `n` (the loop rounds up to 4). It terminates because those lanes are written to
   `pass` too, but the loop can read past `n` — safe only because the span buffers
   carry 16 entries of slack. Fine today; fragile if the padding ever shrinks.

3. **`texture_gather4` fallback ignores the specialised path silently.**
   `select_gather4` (render3d.cpp:1277) returns `nullptr` when the texture or
   palette is not host-contiguous, and `span_texels` (1301) then takes the per-lane
   `texture_sample` loop — the slowest path in the renderer — with only a profile
   counter (`C_TEX_SLOW_VIEWS`) to show it. Since `texture_fields` returns "use the
   decoded cache" for exactly that case (984, `return sh.textured && (sh.fmt == 5 ||
   !sh.tex_ptr || !sh.pal_ptr);`), the fallback should be unreachable whenever the
   cache is enabled. If `C_TEX_SLOW_VIEWS` is ever non-zero outside
   `DS_NO_TEXCACHE`, something is wrong; it is worth asserting rather than
   silently running the slow loop.

4. **Not a bug, but the comment at `render3d.cpp:2129` (item 7) states a
   measurement conclusion without a measurement**, unlike the rest of the file.
   Given how much of this renderer's design rests on recorded A/Bs, that one
   should either get a number or be re-tested.

---

## Checked and found already good (do not re-propose)

- Texel gather is specialised per (format, S wrap, T wrap) and hoisted to one call
  per span (`gatherN_*`, render3d.cpp:1224-1290) — DraStic's nine-kernel shape.
- Constant-attribute and constant-RGB span paths exist and are decided per polygon
  (`Shade::attrs_constant`/`rgb_constant`, render3d.cpp:1046-1057).
- The resolve is one call per batch with the kernel bound in the `Shade`
  (`select_resolve`, 1888) — no indirect call per span.
- The four-lane vs eight-lane split in `resolve_span_vec` (1717-1721) is already
  specialised for short edge runs, and the no-under-layer group is a separate
  instantiation rather than a masked shared body — both are the right call for an
  in-order core, and the comments record the measurements.
- `pass`-plane narrowing by the alpha test in `span_shade` (1373) removes the dead
  groups the resolve used to load.
- Clears are bulk `std::fill` / a fused `clear_image_run` kernel, and the border
  rows are written once per band, not per line.
- Worker synchronisation: the common `sync_line` path is one atomic load
  (`Pool::wait_bits`, 273-285); bins are claimed with a single `fetch_add`; band
  timings are written to per-worker slots (`band_ns_[w]`), so there is no false
  sharing on the hot path.

----------------------------------------------------------------
# FULL REPORT: r2d

# 2D subsystem audit — gpu.cpp / engine2d.cpp / kernels_neon.cpp / vram_map

Target: RG DS (4x A55, in-order, 2x64-bit NEON pipes, 32 KB L1D).
All file:line refs are on the current worktree. Nothing was edited.

Scope note on what is *already* done, so it is not re-proposed: the degenerate
`pa=256, pc=0` affine/extended fast paths exist and are taken
(`bitmap_row_degenerate` engine2d.cpp:884, `tile_row_degenerate` :925), the
text map row is two memcpys not 33 reads (:632), blank-tile and blank-row
early-outs exist (:657, :693), the flat / fade / second-target stage split
exists (render_line :536-556), and the "don't test the mask in the select"
result is recorded in a comment at kernels_neon.cpp:410. Good.

---

## Ranked opportunities

### 1. The text-BG kernels run one tile (64 bits) per iteration with three GPR→SIMD moves each
`kernels_neon.cpp:352 text_row_16`, `:370 text_row_256`.

```cpp
for (u32 t = 0; t < n; ++t, packed += 4, v += 8) {
  u32 w; std::memcpy(&w, packed, 4);
  const uint8x8_t raw = vreinterpret_u8_u32(vdup_n_u32(w));          // GPR -> SIMD
  ...
  const uint8x8_t mask = vbsl_u8(vdup_n_u8((ctl[t] & 0x10) ? 0xFF : 0), ...); // GPR -> SIMD
  vst1q_u16(v, vandq_u16(m, vorrq_u16(i16, vdupq_n_u16(LV_OPAQUE | ((ctl[t] & 0xF) << 4))))); // GPR -> SIMD
```
Why it costs on an A55: each `vdup_n_*` from a general register is a
`dup Vd, Wn` — 4-6 cycle latency, and it sits on the critical path of the
iteration (the scalar `ctl[t]` load feeds it). Three of them per 8 pixels,
33 iterations per layer per line, up to 4 layers, 192 lines, 2 engines. The
whole body is 64-bit wide, so it uses one of the two NEON pipes. The
`vbsl_u8(dup(cond), 0xFF, 0x00)` is also just `dup(cond)` written the long way.
The doc (docs/techniques/03 §6) describes DraStic doing four tiles at a time
with `cmtst`/`rev32`/`sli` and no scalar dependency at all — this is the one
place where the reference design is clearly ahead of us.

Change: process 2 (text_row_256) or 4 (text_row_16) tiles per iteration —
`vld1q_u8` the packed row bytes, `vld1_u8` the `ctl` bytes, build the flip
mask with `vtstq_u8(ctl, 0x10)` expanded by `zip` to per-pixel lanes, and the
palette-OR term with a `zip`/`shl` of `ctl & 0xF`. No GPR→SIMD move in the loop.
Also fold `rowacc`/`any` from the same vectors.

Exactness: bit-exact (same values, different grouping); `tests/kernels_test.cpp`
already diffs ref vs neon vector by vector.
Size: this is, by call count, the hottest 2D kernel on any tiled scene. Honest
estimate 25-40 % off `text_row_*`, ~0.2-0.5 ms/frame on a 4-text-layer scene.
Measure: mlbis and sm64 (2D-heavy, tiled) with DS_PROFILE `BG_DRAW`, and qemu
hotblocks for an exact instruction delta (this is well under the 1 % device
noise floor per the A/B run-order note, so use the census).
Risk/effort: medium-low. Self-contained, covered by the kernel test.

### 2. The per-tile gather loop in `draw_bg_text` is a 33-iteration dependent scalar chain
`engine2d.cpp:660-694` (the `c256` / else gather), plus `:645`:

```cpp
bool uniform = true;
for (u32 t = 1; t < 33; ++t) uniform &= tiles[t] == tiles[0];   // 32 scalar compares, always
...
for (u32 t = 0; t < 33; ++t) {
  const u16 tile = tiles[t];
  ... ctl[t] = ...; palmask |= 1u << (tile >> 12);              // palmask is dead in the 4bpp loop
  const u32 a = tileset + ((tile & 0x3FF) << 5) + (ty << 2), am = a & amask;
  if (const u8* p = vv.ptr[am / VramView::BLOCK]) { std::memcpy(&row, p + ..., 4); }
  ...
}
```
Per tile: ~15-20 instructions and two dependent loads (`vv.ptr[block]`, then
the tile row itself). In-order, so any L2 hit on the tile row stalls the
iteration; the iterations are independent but the scheduler has little room
between them.

Changes, in order of value:
- The block pointer almost never changes across the 33 tiles (a tileset is
  16 KB and the tiles of one map row usually live in one block). Cache
  `last_block`/`last_ptr` and skip the `vv.ptr[]` load — removes one of the
  two dependent loads per tile.
- Hoist the ctl/palmask computation into a vectorised pass over `tiles[33]`
  (it is a contiguous u16 array): `ctl` is `(tile>>12) | ((tile>>6)&0x10)` for
  all 33 in ~6 vector ops, and `palmask` is a reduction. Drop `palmask`
  entirely from the 4bpp loop (it is only read under `c256 && extpal`, :700).
- Replace the 32-compare `uniform` loop with 3 vector compares over `tiles`.

Exactness: bit-exact.
Size: ~15-25 % off the gather, call it 0.1-0.25 ms on a 4-layer scene.
Measure: qemu hotblocks on mlbis/sm64.
Risk/effort: low.

### 3. `rgb555_table()` is a 128 KB gather table
`engine2d.cpp:431` builds `std::array<Pixel, 32768>` and it is installed as a
resolve table for every line (`setup_tables` :1244, and `plane.table` for
direct-colour bitmap BGs :795). `resolve16` / `resolve16_full` / `resolve16_one`
then do a scalar `tab[top[i] & 0x7FFF]` gather per pixel.

Why it costs: 128 KB against a 32 KB L1D and a shared 512 KB L3 — a
direct-colour line's gathers miss L1 essentially every pixel, and on an
in-order core each miss is a full load-use stall (~12+ cycles to L3) with no
other work to hide it. It also evicts the rest of the 2D working set
(planes + top/second + out are ~9 KB per engine) every line it runs. Any
bitmap sprite on the line pulls the same table in through `T_OBJ_DIRECT`.

Change: the conversion is 6 ALU ops (`rgb15_to_18`, :40) and vectorises 8
pixels at a time. Add a `resolve16_direct` path: when the hoisted table
pointer for a block equals the direct table, compute instead of gather
(`resolve16`/`resolve16_top`/`resolve16_full` already hoist per-16 uniform
blocks, so the branch is already there and costs nothing new); `resolve16_one`
gets the same test once per line. Then the 128 KB array can be deleted.
Exactness: bit-exact — it is the same function, inlined.
Size: large *where it applies* (direct-colour BG or bitmap sprites): plausibly
several hundred µs on such a line set, and a cache-footprint win everywhere.
Measure: GSDD (direct-colour path) and any title with bitmap sprites; PMU
L1D refill counters make this one visible directly.
Risk/effort: low. Highest value-per-effort item in this list.

### 4. `composite_line`'s pass-through block pays a full `ld4`/`st4`
`kernels_neon.cpp:139-146`:
```cpp
uint8x16x4_t a = vld4q_u8(reinterpret_cast<const u8*>(top + i));
if (compat::maxv_u8(...) == 0) { a.val[3] = vff; vst4q_u8(..., a); continue; }
```
By the kernel's own comment this is the common case ("most blocks of most
lines blend nothing"). `ld4`/`st4` on 4x16 B is the expensive de/re-interleave
the NEON-census note already identified as our gap against DraStic; here it is
paid purely to force the alpha byte to 0xFF. Two `vmaxvq`+`fmov` readbacks
(`maxv_u8`, :141) precede it.

Change: in the pass-through path do 4x `vld1q_u32` / `vorrq_u32(0xFF000000)` /
`vst1q_u32` and skip the deinterleave entirely; only enter the planar path
once a block has something to blend. Hoist the `ld4` below the test.
Same applies to `composite_line_fade` (:257) — it deinterleaves every block
even though `hit` is empty on most of them.
Exactness: bit-exact.
Size: `composite_line` runs on every effect line of every frame; ~30-50 % off
the kernel on typical content. 0.1-0.3 ms on effect-heavy scenes.
Measure: etody (effect-heavy) + mlbis, DS_PROFILE `EFFECTS`.
Risk/effort: low.

### 5. Cross-lane reductions (`vmaxvq`+`fmov`) used as scalar predicates
`kernels_neon.cpp:493` (`resolve16`), `:246` (`resolve16_top`), `:308-309`
(`resolve16_full`, **four** reductions per 16 pixels), `:141,:145`
(`composite_line`).

On A55 a `umaxv` across 16 bytes plus the `fmov` back to a GPR is a
multi-cycle vector→scalar readback that the in-order pipeline cannot overlap
with the branch that consumes it. `resolve16_full` pays 4 per 16 pixels =
64 per line per engine.

Change: the predicate is "are these 16 bytes all equal", and the bytes are in
memory. Two `ldp` GPR loads + `x ^ (b*0x0101010101010101)` + `orr` + `cbz` —
no SIMD involved, ~5 instructions, no readback. (`compat::uniform_u8`,
neon_compat.h:106, exists for exactly this question and is *used nowhere* —
every call site spells `maxv==minv` by hand. Give it the scalar-GPR body and
route the call sites through it.)
Exactness: bit-exact.
Size: small but broad, ~0.03-0.08 ms/frame.
Risk/effort: very low.

### 6. `resolve16_one` round-trips indices through the stack (store-to-load forwarding stall)
`kernels_neon.cpp:508-524`:
```cpp
alignas(16) u16 ix[8];
vst1q_u16(ix, idx);                       // 128-bit store
lo = vsetq_lane_u32(table[ix[0]], lo, 0); // 16-bit loads from that store, then GPR -> SIMD inserts
... (8 of them)
```
Two A55 hazards in one loop: a 128-bit store immediately followed by 16-bit
loads from the same address will not forward (~10+ cycle penalty per
iteration, 32 iterations per line), and each `vsetq_lane_u32` of a
general-register value is an `ins` from GPR. This kernel is the
single-opaque-layer fast path — i.e. the path taken on the *cheapest, most
common* lines.

Change: skip SIMD entirely. Load the indices as two `u64` GPR loads straight
from `v`, `ubfx` out four 15-bit indices each, `ldr` the four table entries,
`orr` the alpha, and store with `stp`. Eight independent loads in flight,
which is what actually hides the load latency here (the kernel comment already
says that is the intent — the vector wrapper defeats it).
Exactness: bit-exact.
Size: `C_2D_FAST_ONE` lines are a large share of HUD/2D frames; 0.05-0.15 ms.
Measure: DS_PROFILE `SELECT` + the `C_2D_FAST_ONE` counter on mlbis/sm64.
Risk/effort: low.

### 7. Unconditional per-line clears
- `render_sprites` engine2d.cpp:989 clears **four** planes before it knows
  whether anything will be drawn: `obj_v_` (544 B), `obj_attr_` (272 B),
  `obj_alpha_` (272 B), `obj_win_` (256 B) = ~1.3 KB per line per engine, i.e.
  ~500 KB/frame of pure stores. Only `obj_attr_` is load-bearing: every
  consumer of `obj_v_` / `obj_alpha_` is gated on `OA_OPAQUE` in the attribute
  byte (`select16_obj*` masks by it, `setup_tables` :1268 tests it,
  `resolve16_full` masks alpha by `isobj`), and `obj_win_` is only read when
  `dispcnt_ & (1<<15)`. Clear `obj_attr_` always, `obj_win_` only under the
  OBJ-window bit, and drop the other two.
- `build_window_plane` engine2d.cpp:1192 fills `win_` with 0xFF whenever no
  window is enabled — and it is called (render_line :521) *before* the flat
  path is chosen, where `win_` is never read (`select16_flat_nowin` +
  `resolve16`). Skip the fill on no-window lines and give
  `composite_line`/`composite_line_fade` a `nowin` twin (they only test bit
  0x20 of it).
- `draw_bg_extended` :782 / `draw_bg_large` :854 `memset(plane.v(), 0, 512)`
  before the degenerate fast path, which on the wrapping path writes all 256
  entries anyway (`bitmap_row_degenerate` :917 covers `i` 0..255). This is
  GSDD's per-scanline engine-B layer — clear only the uncovered ends on the
  non-wrapping path and not at all when wrapping.
- `select_layers` :1281-1282 fills four arrays (1.5 KB) per line; the
  `second16_`/`second_tid_` pair only when `needs_second()`, which is already
  known. (`select_top_only` :1340 correctly fills only two.)

Exactness: bit-exact, but items 1 and 3 need care — verify with
`tools/scene_hashes.sh` across all five scenes plus DS_DEBUG_OUTHASH (the
composite output feeding capture is not in the frame hash).
Size: ~2-3 KB/line/engine of stores removed ≈ 1 MB/frame ≈ 0.05-0.1 ms, plus
less L1D churn. GSDD gets the extended-BG memset specifically.
Risk/effort: low-medium (the obj_v_ argument needs to be verified, not assumed).

### 8. `DS_2D_SPLIT` is implemented but defaults off — and the open "adaptive trap" lead may just be this
`gpu.h:319 bool split_ = false;` with `store_engines()` (gpu.cpp:274)
returning `3` (both engines) for every address unless it is set. On GSDD the
game streams engine B's BG through per-scanline HBlank DMA; without the split,
each of those stores bursts **engine A** out of batching too, which is exactly
the shape of the recorded "DS_2D_LAZY=0 wins 5.4 % on GSDD" result. The
machinery to charge a store to only the engine whose window it lands in is
already written, tested and exact.

Change: measure `DS_2D_SPLIT=1` on GSDD (and check the other four for
regressions); if it holds, make it the default and keep `DS_2D_SPLIT=0` as
the bisect knob. Only then consider a genuinely adaptive burst policy
(`LAZY_BURST_LIMIT`/`LAZY_BURST_LINES` are fixed constants at gpu.h:340; the
per-engine hit counters needed to adapt them already exist in
`lazy_bursts_[]`).
Exactness: bit-exact by construction — it only changes *when* lines render,
and the journal makes that invisible. `DS_2D_LAZY=0` vs `=1` hash equality is
the existing gate.
Size: potentially the 5.4 % GSDD gap, i.e. ~1 ms there.
Measure: GSDD at `--quantum 0`, both run orders, p99 + over-budget.
Risk/effort: very low effort, and it is measurement rather than new code.

### 9. Sprite row assembly is scalar byte-at-a-time
`engine2d.cpp:1108`:
```cpp
for (u32 i = xoff; i < xend; ++i) row[i - xoff] = idx[hflip ? (w - 1 - i) : i];
```
and the bitmap path `:1067`:
```cpp
for (u32 i = xoff; i < xend; ++i) {
  const u32 sx = hflip ? (w - 1 - i) : i;
  if (row) std::memcpy(col + (i - xoff), row + sx * 2, 2); else ...
}
```
Up to 64 iterations per sprite, with a loop-carried branch on `hflip` and a
2-byte `memcpy` per pixel in the bitmap case. Also `:1097-1103`, the 4bpp
nibble expansion, is 4 scalar unpacks per tile.

Change: split on `hflip` outside the loop — `!hflip` is a `memcpy` of the run;
`hflip` is a `vrev64q_u8` + `vextq` (or `vrev64q_u16` for the bitmap case)
over 16/8 lanes. Expand 4bpp nibbles with `vand`/`vshr`/`vzip` (the same idiom
`text_row_16` already uses) instead of the scalar loop.
Exactness: bit-exact.
Size: proportional to sprite count; on a sprite-heavy scene (Meteos-like, or
mlbis) 0.05-0.2 ms. `OBJ_DRAW` is already a DS_PROFILE stage.
Risk/effort: low.

### 10. Rotscale sprites go through the out-of-line `VramMap::read8`
`engine2d.cpp:1156, 1165, 1145` use `vm.read8(vv, ...)` / `vm.read16(...)`
per pixel. Those are defined in vram_map.cpp (`or_read<T>`, :123) — a full
mask-loop OR-read, and a cross-TU call unless LTO happens to inline it. The
affine BG paths correctly use the inline header fast path
(`vram_fetch8`/`vram_fetch16`, vram_map.h:85-97), which is one array lookup
in the common single-bank case.

Change: use `vram_fetch8`/`vram_fetch16` in the rotscale sprite loops. One-line
change, removes a call and the mask loop from a per-pixel path.
Exactness: bit-exact (the helpers fall back to `read8` for overlapping banks).
Size: small in aggregate, but rotscale sprites are common (Mario Kart, any
rotating HUD element); it also stops relying on LTO for a hot inline.
Risk/effort: trivial.

### 11. The priority select touches six line arrays per layer (the representation gap)
`merge16` kernels_neon.cpp:396-408 — per layer, per 16 pixels: load
`top`/`top+8`, load `second`/`second+8`, load `top_tid`, load `second_tid`,
four `bsl`, six stores. That is a read-modify-write of ~6 planes for every
layer on every line, ~1.3 K memory ops per line per engine at four layers.
DraStic's answer (docs/techniques/03 §3) is a bit-per-pixel visibility mask:
32 bytes per layer, ~14 instructions to resolve first *and* second place for
all 256 pixels, and the value select happens once at the end with whole
32-pixel groups skipped where the mask word is zero.

I am not proposing the full rewrite as a first move — it is a redesign of
`Layer`, the select, and every kernel that consumes `top16_`. But there is a
cheap down payment that gets the "skip empty groups" half without the
`vmaxvq` readback the earlier experiment (comment at :410) rightly rejected:
`text_row_16`/`text_row_256`/`bmp_row_*` already compute per-tile emptiness
for their `any` return — have them also return a 32-bit *tile occupancy mask*
(bit t = tile t has an opaque pixel). The select can then skip 8-pixel groups
by testing bits of a general register, which costs a `tbz`, not a vector
readback. On HUD/text layers, which are mostly empty, that removes most of the
RMW traffic.
Exactness: bit-exact.
Size: hard to call honestly — 0 on a full-screen layer, potentially 30 %+ of
the select on sparse layers. Measure on mlbis (HUD-heavy) with the
`C_2D_SELECTS` counter and DS_PROFILE `SELECT`.
Risk/effort: medium. The full bitmask redesign is a multi-week item; do the
occupancy-mask version first and let it price the rest.

### 12. Per-line and per-frame `getenv`/magic-static guards in hot paths
`ablate()` gpu.cpp:31 is a function-local `static const` — every call is a
guard-variable load and test, and it is called from `step_engine` (:706),
`output_engine` (:748) and `render_ranges` (:632), i.e. 3-4 times per line per
engine. `rgb555_table()` (engine2d.cpp:431) is likewise a guarded static
called from `setup_tables` on every line. `join_worker` (:574) and
`render_ranges` (:620) each hold a `static const bool dbg = getenv(...)`.
`begin_frame` calls `std::getenv` three times per frame (:519, :523, :528).

Change: hoist all of these to plain globals initialised once at start-up
(the env-knob audit already queued is the right home for this), and cache the
`rgb555_table()` pointer in the engine.
Exactness: bit-exact.
Size: small (~0.01-0.02 ms) but free.
Risk/effort: trivial.

---

## Bugs / fidelity-vs-speed observations (separate)

1. **The profiler allocates on the heap per line.** `render_line`
   engine2d.cpp:475 (`new prof::Scope(prof::BG_DRAW)` … `delete sc`),
   `step_engine` gpu.cpp:723 (OBJ_DRAW) and `output_engine` gpu.cpp:735
   (OUTPUT) do a `new`/`delete` pair per line per engine when `DS_PROFILE` is
   on — ~1150 malloc/free per frame. That is a measurable part of the
   +2.9 ms/frame the "untimed is the instrument" note records, and worse, it
   is charged unevenly across stages, so the DS_PROFILE breakdown is skewed
   toward whichever stages use this pattern. Replace with a manually
   constructed scope object in automatic storage (a `union`/`aligned_storage`
   with an explicit `end()`), and the instrument gets both cheaper and more
   honest.

2. **Dead code in `output_a`.** gpu.cpp:1136 `case 1:` copies
   `engine[0].output()` into `dst` — but `output_engine` (:754-757) routes
   display mode 1 to the fused `output_line` kernel and only calls `output_a`
   for modes 0, 2, 3. The `case 1` arm is unreachable. Harmless, but it hides
   the fact that mode 0 then runs `output_a` (a 256-iteration white fill) plus
   a separate `expand_colours` pass, where one `memset`-shaped fill of the
   final 0xAARRGGBB value would do.

3. **`output_b` is three passes over the line.** gpu.cpp:1148-1153: a scalar
   256-element copy, then `apply_master_brightness` (a whole pass), then
   `expand_colours` (another pass) — 3 KB of traffic where the fused
   `output_line` kernel does it in one. This is only the
   engine-B-not-displaying path, so it is cold; noting it for completeness
   since it is the same shape as (2).

4. **`compat::uniform_u8` (neon_compat.h:106) has no callers.** It was written
   because "the kernels ask this question far more often than they ask for
   either extreme", and then every kernel spells out `maxv==minv` anyway. See
   item 5.

5. **Journal append does a release store per guest write.** `Engine2D::queue`
   engine2d.cpp:172 (`jn_.store(n + 1, std::memory_order_release)`) — one
   `stlr` per palette/OAM/register write, thousands per frame on
   palette-animating titles. It is correct as written (main appends while the
   worker replays); a cheaper scheme would batch the release to line
   boundaries, but only if the stamp invariant can be shown to keep the worker
   away from the un-published tail. Flagging it as a known cost, not as a
   safe change.

6. **Cache footprint worth stating explicitly:** per engine the hot 2D
   working set is bg planes 4x544 B + obj planes ~1.3 KB + win 256 B +
   top/second/tids 1.5 KB + top_/second_/out_ 3 KB ≈ 9 KB, which is fine
   against 32 KB L1D — until `rgb555_table` (128 KB, item 3), `extpal18_`
   (64 KB) or `objext18_` (16 KB) is touched. Item 3 removes the worst of it;
   the extended-palette tables are only walked on conversion, so they are
   acceptable.

---

## Suggested order of work

Cheap and near-certain first: **8** (measure DS_2D_SPLIT=1 on GSDD — no code),
**3** (delete the 128 KB table), **10**, **12**, then **4**, **6**, **5**, **7**.
Then the two real engineering items: **1** + **2** (the text path, the largest
steady cost), and finally **11**'s occupancy-mask down payment before deciding
whether the bit-per-pixel redesign is worth funding.

Every item above is bit-exact; gate all of them with `tools/scene_hashes.sh`
on the five scenes under both interp and JIT, and additionally with
`DS_DEBUG_OUTHASH` for items 4/7 (composite output feeds display capture,
which the frame hash does not see). Per the run-order-bias note, use the qemu
instruction census for anything expected under ~1 % and run both orders on
device otherwise, always at `--quantum 0`, reporting p99 and over-budget
frames.

----------------------------------------------------------------
# FULL REPORT: frontend

# Frontend audit — SDL/Wayland/KMS presentation, frame loop, threading

Scope: `src/frontend/sdl/{main,display,display_wl,display_drm,display_fbdev,dmaheap,audio,input,lid,mic_alsa,config,menu,wl_dyn}.cpp`, `scanout.h`, `rt_thread.h`; `src/frontend/headless/main.cpp` for comparison. `display_disp.cpp` (A30) skipped as instructed.

Headline: the per-frame *CPU work* in this frontend is already small and clean — no per-frame `getenv`, no per-frame allocation on the hot path, no logging in the loop, the pause menu costs literally nothing while hidden (it lives entirely inside the `if (paused)` block, main.cpp:1525-1569), config is read once at start-up, and the scaling path stages each row in cached scratch and never reads the panel buffer back. The opportunities are almost all **structural**: where the waits are, how many buffers there are, and who has which scheduling priority. Those are p99 levers, which per `[[check-p99-not-just-mean]]` is what matters here.

---

## 1. The KMS scanout tier has two buffers and an unconditional flip-wait: zero jitter absorption (RANK 1)

`display_drm.h:44` — `static constexpr int BUFS = 2;`
`display_drm.cpp:220-233`:
```cpp
u32* DrmOut::begin_frame() {
  pump(fd_, false);
  while (pending_ >= 0)
    if (!pump(fd_, true)) { dead_ = true; return nullptr; }
  for (int i = 0; i < BUFS; ++i) if (!bufs_[i].busy) { cur_ = i; return bufs_[i].px; }
```
Compare `display_wl.h:46` (`BUFS = 3`) and `DmabufOut::begin_frame` (display_wl.cpp:188-203), which blocks **only when every buffer is busy**.

What this costs on an RK3566 panel: the DRM path waits for the previous flip to *retire* before it will hand out a buffer, whether or not a buffer is free. So the loop is strictly `wait-for-vblank → emulate+scale → queue flip → wait-for-vblank`. There is no pipelining at all: an 18 ms frame does not present late, it presents at 33.3 ms and the emulation thread then sits idle for 15 ms. A scene that alternates 14 ms / 18 ms frames runs at a visible 60/30/60/30 rather than averaging out. `scanout.h:14-17` says the design intent is "the display's pacing is felt before the frame's emulation ... where the frame loop already has slack to absorb it" — but on this tier the buffer count *is* the slack, and it is zero. Wayland has two frames of slack; KMS has none.

Proposal, in increasing order of effort:
1. `BUFS = 3` and move the retire-wait: only `pump(block=true)` when the free-buffer scan comes up empty. `end_frame()` then needs somewhere to put a flip it cannot queue yet — either flip-on-retire from a small presenter thread (the shape `FbdevOut::presenter()` already has, display_fbdev.cpp:206-224, which exists for exactly this reason and is documented as taking the present from 9.2 ms to 0.03), or keep one "queued" index and flush it at the head of the next `begin_frame`.
2. Same treatment for dual-window: with two `DrmOut`s, main.cpp:1692-1693 calls `display.begin_frame()` then `display2.begin_frame()` back to back, so two independent unconditional vblank waits serialise on the emulation thread every frame. If the panels are not phase-locked this is up to two refresh periods per frame.

Exactness: bit-exact — this is presentation only, no core state touched.
Win: does not move the mean; should visibly cut p99 and over-budget frames on any scene whose frame time straddles the refresh. Measure on **etody** (35 % band-wait, the tail scene) and **GSDD** (its 8.5 ms 2D phase / 44 ms 3D phase alternation, `[[gsdd-title-is-two-workloads]]`, is precisely the shape a 2-buffer flip chain quantises worst), with `DS_FRAME_STATS` p99/over-budget and the `DS_FPS` line, on the KMS tier (`SDL_VIDEODRIVER=KMSDRM`).
Risk: moderate. Getting `retire()`/`busy` bookkeeping right with three buffers and one outstanding flip is fiddly; the `close()` drain path (display_drm.cpp:171-174) needs to follow. A presenter thread also re-raises the priority question in item 3.

## 2. Input is sampled before the vsync wait, so it is a full frame stale on the scanout tiers (RANK 2)

main.cpp:1357-1359 polls SDL events at the top of the loop; `display.begin_frame()` — which is where the panel wait lives — is at 1692, ~330 lines and one full input/mic pass later. On the KMS tier that wait is a guaranteed full refresh period (item 1); on Wayland it is a wait whenever the emulator is ahead.

So the controller state applied to `nds` at main.cpp:1618 (`ds::input::apply`) was read up to 16.7 ms before the frame it feeds actually starts emulating. Fix: keep the event drain where it is for the hotkey/menu actions, but re-poll (a second `while (SDL_PollEvent(...))` plus `input.update_stylus()` / `input.frame()`) *after* `begin_frame()` returns and before `apply()`. Or simply hoist `begin_frame()` above the input block.

Exactness: bit-exact for a replay (`log.reading()` path is unaffected); changes live input timing by design.
Win: not a throughput win — one frame of input latency on a handheld, which is worth more to the player than 1 % of frame time. Measure by feel / by `--record` timestamps, not by a benchmark.
Risk: low. Watch that `pause_pending`/`shot_pending` (which gate whether `begin_frame` is called at all, main.cpp:1691) are still decided before the hoist.

## 3. Every thread inherits SCHED_RR at the same priority, nothing is pinned, and the compositor is not one of ours (RANK 3)

main.cpp:697-706:
```cpp
const std::string rt = cfg.str("emu.realtime", "rr");
const int prio = cfg.num("emu.rt_priority", 5);
sched_param sp{}; sp.sched_priority = prio;
sched_setscheduler(0, rt == "rr" ? SCHED_RR : SCHED_FIFO, &sp)
```
This runs before `NDS nds;` and before SDL creates anything, and pthreads default to `PTHREAD_INHERIT_SCHED`. So the emulation thread, the `LineWorker` (line_worker.h:48), the 2-3 band workers (render3d.cpp:216), SDL's audio thread and SDL's Wayland/evdev threads all land at RR priority 5 — five to seven RT threads on four A55s, with no `sched_setaffinity` anywhere in the tree.

Two consequences the code has already discovered once and only half-fixed:
- `rt_thread.h:1-11` documents exactly this failure for the A30 presenter ("a round-robin slice on a 3.4 kernel is 100 ms, so the panel holds a stale frame for that long"), and `raise_presenter_priority()` is applied to the fbdev and disp presenters — but **nothing raises SDL's audio thread**, which is the frontend's actual frame clock (`Audio::pace()`, main.cpp:1883). An audio thread starved behind three flat-out RT band workers stops draining the queue, `pace()` sees the queue above target and sleeps, and the emulator throttles itself on a clock that is stalled for scheduler reasons. The `stalled_` machinery in audio.cpp:142-153 is a symptom-catcher for this class of event, not a cure.
- On Wayland the **compositor is a different process at SCHED_OTHER**. `DmabufOut::begin_frame` (display_wl.cpp:196-201) blocks on a `wl_buffer.release` that only sway can send. Filling four cores with RT threads means sway cannot run to send it, and what unwedges us is the kernel's RT throttle (`sched_rt_runtime_us`, 950 ms in 1 s by default) — a 50 ms hiccup. This is a plausible, testable explanation for tail frames on the Wayland tier that no amount of core optimisation will touch.

Proposal: differentiate the priorities instead of flattening them. Emulation thread at `prio`; band workers and the LineWorker at `prio` (they are what the emu thread waits on, so do not lower them) but **capped to `cores - 1` when a compositor is in the session**; SDL's audio thread and any presenter at `prio + 1` (the `raise_presenter_priority()` idiom, applied via `SDL_SetThreadPriority`/a hook, or by opening the device before the `sched_setscheduler` call and raising it explicitly). Also consider `sched_setaffinity` to pin emu + workers to distinct cores rather than letting the RT scheduler migrate them and cold-start L1 each time.

Exactness: bit-exact.
Win: honestly unknown, but the existing note in main.cpp:690-696 says the *first* RT change moved Golden Sun p90 16.8→14.7 and p99 23→18, so the scheduling axis has already been shown to be worth more than most code changes here. Measure with `DS_FRAME_STATS` p99 + over-budget on **etody** and **GSDD**, on the Wayland tier with audio on, and repeat with `chrt`-adjusted priorities before writing any code — this is a one-command experiment.
Risk: low to try, medium to ship (needs a fallback where the privilege is refused, which the code already handles).

## 4. `Audio::pace()` busy-polls the SDL device lock while holding an RT core

audio.cpp:154-167:
```cpp
const Uint32 deadline = SDL_GetTicks() + 100;
while (SDL_GetQueuedAudioSize(dev_) > limit) {
  if (SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) { ... }
  SDL_Delay(1);
}
```
`SDL_GetQueuedAudioSize` takes the device lock in SDL2, so each iteration contends with the audio thread we are waiting on. At a 3-frame target the loop typically runs a handful of iterations, but on a fast frame it can run 8-10, i.e. 8-10 lock/unlock pairs and 8-10 `nanosleep`s at RT priority while the very thread that would drain the queue wants a core (item 3).

The queue depth is known and the drain rate is known exactly (`frame_bytes_`, audio.cpp:39), so the wait is computable: sleep once for `(queued - limit) / bytes_per_ms`, then re-check. One syscall instead of ten, and no lock storm.

Exactness: bit-exact. Win: small in absolute time (tens to low hundreds of microseconds a frame), but it removes an RT-priority spin that interacts with item 3. Risk: very low. Measure: `strace -c` frame counts, and the `audio queued %.1f frames` field of `DS_FPS`.

## 5. Translucent PiP inset blends by reading the scanout buffer back

display.cpp:757-775, `blit_insets()`:
```cpp
if (inset_alpha_ == 255) std::memcpy(dst, src, ...);
else blend_row(dst, src, static_cast<size_t>(x1 - x0), inset_alpha_);
```
`blend_row` (display.cpp:744-756) does `dst[i]` reads. The comment at 750-756 is candid that this reads uncached/WC scanout memory and argues it is cheap because the inset is small. But main.cpp:1678-1687 ramps `pip_alpha` toward 255 on touch and back down over ~10 frames to `Layout::pip_alpha`, so any user who sets `pip_alpha < 1` is in the blending path *permanently*, not occasionally. On a 640x480 view a default 1/3-size inset is ~34k pixels of read-modify-write against a write-combining mapping; at plausible WC read rates that is a few hundred microseconds to over a millisecond, on the main thread, in `end_frame()`.

Proposal: keep a cached shadow of the destination rect. The large screen's rows for the inset's rect are produced by the scaler from `row_scratch_` anyway; having `Gpu::output_engine` also drop those rows into a small cached side buffer (or, cheaper, having `targets()` hand the *large* view a staged rect there) turns the blend into cached-read + WC-write.

Exactness: bit-exact (same arithmetic, same result).
Win: only for `pip_alpha < 1`, but potentially ~1 ms there. Measure with a PiP layout and `video.pip_alpha = 0.75` on **etody**, `DS_FRAME_STATS` work series, A/B against `pip_alpha = 1`.
Risk: low, but it is a cross-boundary change into `Gpu` and it only pays for one layout setting — verify the size of the effect with a quick `pip_alpha` A/B *before* building anything.

## 6. No dma-buf cache maintenance anywhere; the code assumes an uncached mapping

`grep -rn "DMA_BUF_IOCTL_SYNC" src/frontend/` finds nothing. Both scanout tiers `mmap(... MAP_SHARED ...)` a dma-heap fd (display_wl.cpp:115, display_drm.cpp:77) and write into it with no `DMA_BUF_IOCTL_SYNC{START,END}` bracketing. dmaheap.cpp:133-137 and 171-179 say the intent is "uncached where the heap has the choice", and display.cpp's comments assert the destination is uncached.

This is a correctness question with a performance answer either way, and it should be *verified on the RK3566*, not assumed:
- If the chosen heap really hands back an uncached/write-combining mapping, everything is correct and the current row-staging design (build in cached `row_scratch_`, one `memcpy` out — gpu.cpp:1043-1093) is exactly right for it. Nothing to do but confirm.
- If it hands back a **cached** mapping (which mainline `cma_heap_mmap` can, unlike `system_heap`'s explicit `pgprot_writecombine` for its uncached variant), then the display is reading stale lines and the only reason it looks right is that the writes happen to be evicted in time. That is a latent tearing/stale-frame bug, and the fix (a sync ioctl pair per frame) is two syscalls, cheap.

How to check: `/proc/<pid>/smaps` for the mapped region, or a one-off microbenchmark writing 4 MB into the mapping and comparing throughput against a `malloc`'d buffer — the memory index already names `cma_blit_bench` as the tool for the CMA write-throughput question. Also log which heap `dmaheap::chosen()` picked (the `buffers from` line already prints it) so the answer is recorded per device.

Effort: an hour. Risk: none to investigate.

## 7. The scanout wait is invisible to both frame-time series (measurement, not perf)

main.cpp:1698 `t0` is taken **after** `begin_frame()`, so the wait for a free scanout buffer / flip retire lands outside both `frame_ms` (t0..t1) and `work_ms` (t0..t2), and outside `draw_ticks`/`pace_ticks`. The comment at main.cpp:1202-1206 explains this as deliberate for the *renderer* tier, where the block was in the present — but on the scanout tiers the block has moved to the head of the loop, so:
- `DS_FPS`'s `present %.1f ms` under-reports the real cost of presenting on exactly the tiers that ship.
- `work_ms`, documented at main.cpp:1949-1951 as "what a missed display frame actually is", no longer contains the display wait at all, and the `--no-vsync` advice there does not apply to the DRM/dmabuf tiers (neither honours `vsync`; `DrmOut` has no vsync flag and `DmabufOut` paces purely on buffer release).
- Separately: `headless` (`src/frontend/headless/main.cpp:389`) never sets a scale target, so its `frame_ms` excludes the entire per-scanline scaling write into the panel buffer, which on the SDL path is *inside* `run_frame()`. Headless and SDL `emu` numbers are therefore not comparable, and a scaling-path regression is invisible to every headless benchmark.

Proposal: add a third accumulator around `begin_frame()` (call it `wait_ms`) and print it beside `present` and `pacing`; and note the headless/SDL caveat in `frame_report`'s legend. Cheap, and it stops the next person concluding "the present is free" from a number that no longer measures it.

## 8. Buffer-acquire failure in dual-window leaks a locked surface / busy buffer (bug)

main.cpp:1692-1693:
```cpp
scaled = display.begin_frame(target);
if (dual_window) scaled = display2.begin_frame(target) && scaled;
```
If `display.begin_frame()` succeeds and `display2.begin_frame()` fails, `scaled` is false, `set_scale_targets(target, false)` drops the targets, and the `else` branch at main.cpp:1794-1796 calls `display.draw(fb)` — which returns immediately on a scanline tier because `ren_` is null (display.cpp:369-370, and `[[display-draw-has-no-renderer]]` records this trap). `display`'s `end_frame()` is never called: on the shm path the surface stays locked (`SDL_LockSurface` at display.cpp:840 never paired), on the scanout path `frame_px_` dangles into a buffer nobody will release, and the panel freezes for the rest of the session.

Fix: if either `begin_frame` fails, `end_frame()` the one that succeeded (or better, acquire both before committing to `scaled`). Rare path, but the failure mode is a hard freeze rather than a glitch.

## 9. Smaller, and things that are *not* levers (so nobody spends a day on them)

Worth a look, cheap:
- **`Lid::poll()` every frame** (lid.cpp:65-83, called from main.cpp:1578): a non-blocking `read()` on the evdev fd plus two `clock_gettime(CLOCK_BOOTTIME/MONOTONIC)` — the suspend-detection skew check runs unconditionally even when a real lid switch exists (`fd_ >= 0` makes the pulse unreachable, so the two clock reads are pure waste in that case). Move the skew check behind `fd_ < 0`. Microseconds, but free.
- **Repeated-row `memcpy` count** (gpu.cpp:1091-1093, 1121-1123): at scale N each source line issues N separate `memcpy`s of one row into the panel buffer. Where the destination rows are contiguous (pitch == run width, i.e. a full-width view with no letterbox) the classic doubling trick (copy 1 row, then `memcpy` 1→2, 2→4) halves the call count and lets the WC write stream run longer per call. Marginal; only worth it if a PMU run shows store-buffer stalls at the row boundaries.
- **`LineWorker` false sharing** (line_worker.h:163-164): `req_`, `ack_`, `quit_`, `parked_` are four adjacent atomics in one 64 B line. The main thread's `req_.store` (seq_cst → `stlr`) invalidates the line the worker is spinning on for `req_`, which is intended, but it also invalidates `ack_`, which the main thread then spins on. Padding `ack_` onto its own line is a five-line change. This is core-owned code, and `[[lineworker-spin-fix]]` records that a previous change here measured a 1 % *loss*, so treat as low confidence — but padding is a different axis from blocking-vs-yielding and has not been tried.

Confirmed **not** worth chasing (checked, negative):
- **Per-frame syscalls**: I counted roughly 10-20 on the KMS tier (one `SDL_PollEvent` drain, one evdev `read`, `poll`+`read` for the flip event, one `PAGE_FLIP` ioctl, `SDL_QueueAudio`, a few `SDL_GetQueuedAudioSize`, `SDL_GetPerformanceCounter` ×6 which are vDSO). Tens of microseconds total. Not a lever.
- **Per-frame `getenv` / config lookups**: none. Every `getenv` in the frontend is at start-up or behind a function-local `static` (main.cpp:1605, line_worker.h:128). Config is read once into locals before the loop (main.cpp:1233-1262).
- **Logging in the loop**: none. `VLOG` is verbose-gated, `DS_FPS` fires 1 frame in 60, the frameskip explanations are one-shot latched.
- **Pause menu while hidden**: costs nothing; the whole menu block including `menu.dirty()` is inside `if (paused)` (main.cpp:1420-1569) and the loop `continue`s out of it.
- **Overlay buffers**: `cursor_fb`/`osd_fb`/`flash_fb[2]`/`menu_fb[2]` are ~1 MB of `std::vector` allocated once (main.cpp:965, 1263-1265) and touched only on the *unscaled* path; on the scanline tiers the cursor/label/flash are drawn straight into the target (main.cpp:1758-1764). No per-frame allocation, no per-frame copy.
- **`take_actions()` returning a vector by value** (input.h:47): swaps with an empty vector, so no allocation on the ordinary frame.
- **`clear_margins`** (display.cpp:700-720): correctly amortised — tracked per buffer index via `out_clean_` (display.cpp:817), not per frame, and the reasoning in the comment about why an index and not a count is right.

## Ranked summary

| # | Item | Exact | Est. win | Effort/risk |
|---|------|-------|----------|-------------|
| 1 | `DrmOut` BUFS=2 + unconditional flip-wait → 3 buffers, deferred wait (or presenter thread) | yes | p99 / over-budget, not mean; potentially large on KMS | med |
| 2 | Input sampled before the vsync wait | yes | up to 1 frame of latency | low |
| 3 | Flat SCHED_RR across 6-7 threads, no affinity, audio thread not raised, compositor starved | yes | unknown, but this axis already bought p99 23→18 once | low to test |
| 4 | `Audio::pace()` lock-spin → computed single sleep | yes | tens-hundreds of µs + removes RT spin | low |
| 5 | Translucent PiP blend reads WC scanout memory | yes | ~1 ms, but only when `pip_alpha < 1` | low-med |
| 6 | No dma-buf cache maintenance — verify the mapping is really uncached | n/a | correctness; possibly none | investigate |
| 7 | Scanout wait excluded from both frame-time series; headless excludes scaling entirely | n/a | measurement integrity | low |
| 8 | Dual-window `begin_frame` half-failure leaks a locked surface (bug) | n/a | freeze on a rare path | low |
| 9 | `Lid::poll` skew check, repeated-row memcpys, `LineWorker` padding | yes | marginal each | low |

----------------------------------------------------------------
# FULL REPORT: periph_build

# Audit: peripherals (SPU / cart / cheat) + build & cross-cutting

Repo root: `/mnt/6d51a940-accf-4ba4-8171-4b05895597c9/Repos/dsperate-project/DSperate`
All findings verified against the actual source and, where marked, against real
cross-compiled AArch64 disassembly (`aarch64-linux-gnu-g++ 13.3.0`, the toolchain
in `cmake/aarch64-linux-gnu.cmake`).

Headline: **the build is the biggest untapped lever in this half of the audit.**
The AArch64 build is compiled for a generic ARMv8-A with the distro's default
`_FORTIFY_SOURCE=3` and `-fstack-protector`, and there is no PGO despite a
perfect deterministic training harness already existing. The peripheral code is
in good shape; the SPU has one real structural item worth ~0.3-0.7 %.

---

## Ranked opportunities

### 1. The AArch64 build is not tuned for the A55 at all — `CMakeLists.txt:64-73`

**Now.** There is no `-mcpu`, `-mtune` or `-march` anywhere for AArch64. Verified:

```
$ grep -rn "mcpu\|mtune\|march=" CMakeLists.txt cmake/ CMakePresets.json
cmake/arm-linux-gnueabihf.cmake:  -march=armv7-a -mfpu=neon -mfloat-abi=hard   # ARM32 only
```

and from a real configured tree, `build/aarch64/build.ninja`:

```
FLAGS = -O3 -g -DNDEBUG -std=c++17 -flto=auto -fno-fat-lto-objects
        -fstack-protector -Wall -Wextra -Wno-unused-parameter
```

So every AArch64 object is built for `-mcpu=generic`, i.e. baseline ARMv8.0-A
with GCC's *generic* scheduling model.

**Why it costs on an A55.** The A55 is in-order and 2-wide. On an in-order core
the compiler's instruction *scheduler* is not a micro-optimisation, it is the
whole game: a load scheduled one instruction before its use stalls 3-4 cycles
(L1) or 12+ (L3) with nothing to fill the slot, and the generic model is tuned
for out-of-order cores where that does not matter. `-mcpu=cortex-a55` switches
GCC to the `cortexa55` pipeline description and also unlocks ARMv8.2-A:
`+lse` (single-instruction atomics instead of `ldxr/stxr` retry loops),
`+dotprod`, `+fp16`, `+crc`, `+rcpc`. The renderer kernels and the byte-plane
composite are exactly the code an in-order scheduling model helps most.

**Change.** In `CMakeLists.txt`, next to the existing ARM32 block:

```cmake
if(DSPERATE_HOST_AARCH64)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-mcpu=cortex-a55" DSPERATE_A55_OK)
  if(DSPERATE_A55_OK)
    add_compile_options(-mcpu=cortex-a55)   # or -mtune=cortex-a55 for a portable binary
  endif()
endif()
```

Caveat, and it matters for how you ship: `-mcpu=cortex-a55` raises the *baseline
ISA*, so the binary will SIGILL on an ARMv8.0 device. If one binary must run on
the RG DS *and* the Miyoo-class hardware, use `-mtune=cortex-a55` (scheduling
only, no new instructions, still most of the win on an in-order core) or gate
the full `-mcpu` behind a device preset.

**Exactness.** Bit-exact for integer code, which is everything here — no
`-ffast-math`, no float reassociation is enabled by this. The float paths in the
3D pipeline are unaffected because `-mcpu` does not change FP semantics.

**Size / measurement.** Honest guess 2-6 % on the main thread; NEON-heavy and
branchy scalar code will move the most. Note `build/fl-a55`, `build/fl-o3mcpu`,
`build/fl-lto` etc. already exist as configured trees, so somebody set up this
sweep and (as far as the docs show) never wrote up a result — re-run it. Measure
per the house rules: dbori for the mean (`gsdd-noise-floor`), GSDD/etody for p99
and over-budget frames, `--quantum 0`, both run orders (`ab-run-order-bias`).

**Risk / effort.** Very low effort, one CMake block. Risk is only the ISA-baseline
question above.

---

### 2. `_FORTIFY_SOURCE=3` turns 2-byte memcpys in the sprite fetch loop into libc calls — `src/core/gpu/engine2d.cpp`

**Now.** Ubuntu/Debian GCC defines `_FORTIFY_SOURCE 3` by default at `-O2`+, and
nothing in the build turns it off. Verified:

```
$ echo | aarch64-linux-gnu-g++ -O3 -dM -E -x c++ - | grep -i fortify
#define _FORTIFY_SOURCE 3
```

The consequence is not theoretical. `Engine2D::draw_sprite_normal` compiles to
this, *inside* its per-tile-row fetch loop:

```
5380: d2800042   mov  x2, #0x2          // n = 2 bytes
5384: 94000000   bl   0 <__memcpy_chk>
5388: 91000b9c   add  x28, x28, #0x2
...
5398: 54fffe81   b.ne 5368 <draw_sprite_normal+0x2c8>
```

and again at `5230` with `n = 8`. Eight such sites in `engine2d.cpp`:

```
[default]              engine2d.cpp memcpy_chk=8
[-D_FORTIFY_SOURCE=0]  engine2d.cpp memcpy_chk=0
```

**Why it costs on an A55.** A two-byte copy that should be `ldrh`/`strh` becomes:
compute the remaining object size (`csel`), set up four argument registers, an
indirect PLT call into libc, `memcpy`'s own size dispatch, and a return — inside
a loop that runs once per pixel-pair per sprite row. Every one is a call/return
pair the A55's branch predictor and 2-wide in-order frontend has to absorb, and
`draw_sprite_normal` is called once per visible sprite per scanline (up to 128
sprites × 192 lines on a sprite-heavy scene).

**Change.** `add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)` in
`CMakeLists.txt` (both must be given; `-U` alone is enough on GCC but the pair is
portable). This is a bounds-check hardening feature on an offline emulator that
reads its own VRAM arrays; the security value here is close to nil.

**Exactness.** Bit-exact — `__memcpy_chk` and `memcpy` compute the same bytes.

**Size / measurement.** Small but free: the 2D path is ~2.4 ms of GSDD's main
thread (`render-ablation-ds-ablate`) and mlbis is sprite-heavy. Guess 0.3-1 % on
sprite-heavy scenes, ~0 elsewhere. Measure with `DS_ABLATE` bit 8 (engine-A draw)
on mlbis and GSDD, or a qemu hotblocks census on `draw_sprite_normal`.

**Risk / effort.** Trivial. This one has no downside I can see.

---

### 3. No PGO, despite the ideal training harness already existing — `CMakeLists.txt`, `tools/scene_hashes.sh`

**Now.** No `-fprofile-generate` / `-fprofile-use` anywhere in the build. Yet
the memory note `jit-code-is-stall-bound` records the measurement that matters
here: **on the RG DS our JIT issues *fewer* host instructions per frame than
DraStic's but runs at IPC 0.29 vs 0.55, with frontend stalls at 44 %.** That is
precisely the profile PGO exists to attack, and it is orthogonal to every
instruction-count lever the audit has already ruled out.

**Why it costs on an A55.** 32 KB L1I, in-order, ~8-cycle mispredict. Without a
profile GCC cannot do hot/cold splitting (`-freorder-blocks-and-partition` only
does real work with profile feedback), cannot lay out branches so the fallthrough
is the taken side, and inlines by static heuristics. The result is cold error
paths, `fprintf` diagnostic blocks, and rare opcode handlers sitting inline in
hot `.text` and evicting icache lines the dispatch loop needs. That is a direct
frontend-stall generator.

**Change.** Two-pass build. The training harness is already there and is
deterministic: `src/frontend/headless/main.cpp` supports `--replay <scene>.dsin`
`--frames N` `--quantum 0` over five recorded scenes, and `tools/scene_hashes.sh`
proves the runs are reproducible frame-for-frame.

```
cmake -B build/pgo-gen -DCMAKE_CXX_FLAGS="-fprofile-generate -fprofile-update=atomic" ...
for s in sm64 mlbis etody dbori gsdd; do <headless> --replay scenes/$s.dsin --quantum 0 ...; done
cmake -B build/pgo-use -DCMAKE_CXX_FLAGS="-fprofile-use -fprofile-correction -Wno-missing-profile" ...
```

`-fprofile-update=atomic` is required: the renderer is multi-threaded
(`render3d.cpp` worker pool) and non-atomic counters would be corrupt.
Training can run under `qemu-aarch64-static` (the toolchain file already wires
`CMAKE_CROSSCOMPILING_EMULATOR` for exactly this) so it costs no device time —
but note `empty-bios-destroys-jit-blocks`: the training runs **must** pass
`--bios9/--bios7/--firmware` or the profile will describe 1-instruction JIT
blocks and be worse than useless.

**Exactness.** Bit-exact. PGO changes layout and inlining, not semantics.
Gate it with `tools/all_scene_hashes.sh` anyway.

**Size / measurement.** This is the item with the largest honest upside in my
half: 3-10 % is the normal range for a branchy interpreter/emulator at 44 %
frontend stall, and the closest comparable (DraStic at IPC 0.55) suggests real
headroom. Measure with the PMU on `.20`: `frontend_stall` / `inst_retired` /
`l1i_cache_refill` before and after, plus the usual p99 table.

**Risk / effort.** Medium effort (a build script and a CI story: the profile has
to be regenerated whenever the source moves enough, and GCC warns loudly when it
does not match). Low risk. If the two-pass build is too much process, the
cheap subset is `-freorder-blocks-and-partition` plus `-ffunction-sections
-Wl,--gc-sections`, but without a profile that buys size, not speed.

---

### 4. `-fstack-protector` on per-line 2D functions — `CMakeLists.txt:70`

**Now.** `add_compile_options(-fstack-protector)` is applied to every target.
Verified which functions actually pay for it:

```
engine2d.cpp  -fstack-protector    chk=25 insns=15039
engine2d.cpp  -fno-stack-protector chk=0  insns=14834
render3d.cpp  -fstack-protector    chk=20
```

The protected functions in the hot files are, by name from the disassembly:
`Engine2D::draw_bg_text`, `Engine2D::tile_row_degenerate`,
`Engine2D::draw_sprite_normal`, `Renderer3D::render_shadow_mask_line`.
All four are per-line or per-sprite. `draw_sprite_normal`'s prologue:

```
50b8: 90000000  adrp x0, __stack_chk_guard
50bc: f9400000  ldr  x0, [x0]
50d4: f9400008  ldr  x8, [x0]        // dependent load, in-order stall
...
53f0: adrp/ldr/ldr/subs/b.ne         // epilogue check
```

**Why it costs.** Two chained loads (`adrp`→`ldr`→`ldr`) at entry, a store, and
a load + compare + branch at every exit, per call. On an in-order core the
second load is a load-use stall on the first. `draw_sprite_normal` runs up to
128× per scanline; `draw_bg_text` up to 4× per line per engine.

**Change.** `-fno-stack-protector`, at minimum on `dsperate_core`. Note
`-fstack-protector` (not `-strong`) was chosen deliberately, but it still fires
on exactly the functions with the big local pixel buffers, i.e. the hot ones.

**Exactness.** Bit-exact.

**Size / measurement.** Small: order 0.1-0.3 % on 2D-heavy scenes. Take it
together with items 2 and 1 as one "build flags" A/B rather than measuring it
alone — it is under the noise floor on its own (`gsdd-noise-floor`: ~1.5 %
irreducible). The `build/fl-o3nosp` / `build/fl-o3sp` trees already exist for it.

---

### 5. SPU mixes all 16 channels per sample even when 12 are silent — `src/core/spu/spu.cpp:350-390`

**Now.** `mix()` runs once per output sample (546/frame) and unconditionally
touches every channel:

```cpp
const s32 ch0 = run_channel(ch_[0], TIMER_STEP), ch1 = run_channel(ch_[1], TIMER_STEP);
...
for (int i = 4; i < 16; ++i) pan_out(ch_[i], run_channel(ch_[i], TIMER_STEP));
```

`run_channel` (`spu.cpp:252-278`) early-outs at
`if (!(c.cnt & 0x80000000u)) return 0;` — but only *after* the call has been set
up, and `pan_out` then still runs on the returned zero:

```cpp
left  += static_cast<s32>((static_cast<s64>(v) * (128 - c.pan)) >> 10);
right += static_cast<s32>((static_cast<s64>(v) * c.pan) >> 10);
```

i.e. two 64-bit multiplies, two `c.pan` loads and two adds per *silent* channel
per sample. That is 546 × 16 = 8,736 `run_channel` bodies and 32 pan multiplies
per sample regardless of how many voices a game actually has running.

**Why it costs.** Pure instruction count on the main thread: roughly 150-250 k
instructions/frame spent proving nothing happened. `docs/techniques/05` §2
records that DraStic solves this the other way round — loops **channels
outside, samples inside**, so channel state is loaded 16× per frame and not 16×
per *sample*, and pre-multiplies volume+pan into two shorts at
`spu_update_channel_settings` so the per-sample body is two `smull`s.

**Change, in two steps of increasing ambition.**
(a) *Cheap and obviously exact*: keep an `u16 active_` mask, set in
`Spu::set_cnt` on key-on and cleared where the code already does
`c.cnt &= ~0x80000000u` (`spu.cpp:202, 212, 234`), and iterate only set bits.
Skipping a disabled channel is exactly equivalent: `run_channel` returns 0 and
`(0 * anything) >> 10 == 0`.
(b) *Structural*: since a batch (`batch_`, default 16, `spu.h:113`) runs inside
one scheduler event with the CPUs stopped and every register access calling
`catch_up()` first, a batch of N samples can be mixed **channel-outer**: produce
N samples per active channel into a scratch buffer, then combine. Nothing can
observe the reordering — the FIFO refills go through `nds_.bus.dma_read32`,
which nothing else writes mid-batch. Capture is already forced to a batch of one
(`ev_mix`, `spu.cpp:329`), so the capture path is untouched.

**Exactness.** (a) bit-exact by construction. (b) bit-exact provided the batch
boundary is respected exactly as `run_to` already defines it — gate with
`tools/scene_hashes.sh` plus `DS_SPU_BATCH=1` and `tools/compare_audio.py`.

**Size / measurement.** Honest: **small.** DraStic's whole SPU is 1.1 % of cycles
(`docs/techniques/00`, `05` §intro) and ours is structurally worse but not by an
order of magnitude — call it 0.3-0.7 % of the main thread, of which (a) recovers
maybe half for an hour's work. `DS_PROF(SPU)` is already in `mix()`
(`spu.cpp:351`), so `DS_PROFILE` gives the before/after directly. Use a
music-heavy scene; mlbis is the halt-heavy one where SPU/interleave already
moved the median (`mlbis-device-ab-2026-08-28`).

**Risk / effort.** (a) low/low. (b) medium/medium — do (a) first and only do (b)
if `DS_PROF(SPU)` says the remainder is worth it.

---

### 6. `Cart::rom_read32` assembles every word byte by byte — `src/core/cart/cart.cpp:168-177`

**Now.**

```cpp
u32 Cart::rom_read32() {
  const u32 hi = rom_addr_ & rom_mask_ & ~0xFFFu;
  u32 lo = rom_addr_ & 0xFFF;
  if (hi != page_base_) { page_base_ = hi; page_ = rom_->page(hi); }
  u32 v = 0;
  for (int i = 0; i < 4; ++i) { v |= static_cast<u32>(page_[lo]) << (8 * i); lo = (lo + 1) & 0xFFF; }
  rom_addr_ = hi | lo;
  return v;
}
```

Four dependent byte loads, four shifts, four ORs and four masked increments per
32-bit cart word, because the address wraps inside its 4 KB page. The wrap can
only bite in the last three bytes of a page.

**Change.**

```cpp
if (lo <= 0xFFC) { std::memcpy(&v, page_ + lo, 4); lo += 4; }   // little-endian host
else { /* existing byte loop */ }
```

**Exactness.** Bit-exact (the emulator already assumes a little-endian host
throughout — e.g. `Spu::fifo_read`'s `memcpy` at `spu.cpp:181`).

**Size / measurement.** ~3× on `rom_read32`, but this path is only hot during
loading and streaming reads, not in the steady state of any of the five
benchmark scenes. It will show up as a shorter load hitch and on GSDD's DMA
census (`gsdd-qemu-instruction-census` puts DMA+GX at 37.9 %), not on a frame
mean. Take it because it is five minutes of work, not because it is a lever.

**Risk / effort.** Trivial.

---

### 7. The cheat engine rescans the whole game's code list on every ARM7 IRQ — `src/core/cpu/cpu.cpp:163-166`, `src/core/cheat/ar_engine.cpp:214-230`

**Now.** `check_irq()` runs the engine from *inside the IRQ path* (JIT included):

```cpp
if (which == Cpu::ARM7 && !nds->cheats.codes.empty()) {
  const io::CpuIo& io7 = nds->io.cpu_io[static_cast<int>(Cpu::ARM7)];
  if ((io7.if_ & io7.ie) & (1u << io::IRQ_VBLANK)) nds->cheats.run(*nds);
}
```

and `Engine::run` walks the entire vector:

```cpp
for (size_t i = 0; i < codes.size(); ++i) {
  const Code& c = codes[i];
  if (!c.enabled) continue;
```

`src/frontend/sdl/main.cpp:444` loads **every** code for the game from
`usrcheat.dat` (`nds.cheats.codes = cheats.codes;`), notes and headings included
(`Code::is_note()`). `Code` is two `std::string`s + a `std::vector<u32>` +
flags ≈ 88 bytes. A game with a thousand database entries means ~88 KB of
strided pointer-chasing per invocation — and the comment at `cpu.cpp:158-162`
explicitly acknowledges the engine "may run more than once in a frame".

**Why it costs.** 88 KB does not fit in the A55's 32 KB L1D, so this evicts the
working set of whatever was running, several times a frame, to discover that
zero or two codes are enabled. It also drags cold code into the icache on the
IRQ path, which is shared with the JIT's exception entry.

**Change.** Keep a `std::vector<u32> enabled_` (indices) rebuilt whenever the
menu toggles a code, and have `run()` iterate that; make the guard
`if (enabled_.empty()) return;` rather than `codes.empty()`. Also fix the guard
in `cpu.cpp:163` to test the enabled count, so a game with a loaded-but-all-off
database costs literally nothing.

**Exactness.** Bit-exact — the order of enabled codes is preserved.

**Size / measurement.** Zero on the benchmark scenes (no cheats loaded) but
potentially several percent for a user with a full `usrcheat.dat`, which is the
normal case for the audience this build is for. This is a "don't ship a cliff"
fix rather than a benchmark item. Measure by loading a large `usrcheat.dat` with
nothing enabled and comparing p99 on any scene.

**Risk / effort.** Low/low.

---

### 8. Zero `__restrict` and zero `__builtin_expect` / `[[likely]]` in the entire source tree

**Now.**

```
$ grep -rn "__restrict" src/ | wc -l          -> 0
$ grep -rn "__builtin_expect|[[likely]]" src/ -> 0
```

**Assessment, honestly.** I am flagging this for completeness, not because I
think it is a big lever, and I want to be clear about which half is which:

- `__builtin_expect` / `[[likely]]`: **do not chase this manually.** It is
  exactly what PGO (item 3) does automatically, correctly, and everywhere,
  and hand-annotation is how you get it wrong. Skip this and do item 3.
- `__restrict`: worth trying in a *targeted* way on the byte-plane composite
  and scanline-scaling loops, where source and destination are distinct member
  arrays the compiler cannot prove do not alias, so it re-loads across stores.
  It is much less useful in the NEON kernels, which are intrinsics and already
  express their loads explicitly. This is the raster subsystem's call, not
  mine, but the global count of zero says nobody has tried it.

**Exactness.** `__restrict` is a promise, not a semantic change — but a *wrong*
promise is silent UB. Gate any use with `tools/all_scene_hashes.sh`.

**Risk / effort.** Low effort, medium risk (aliasing bugs are silent).
Rank below items 1-4.

---

## Things I checked and found *clean* — do not spend time here

- **No virtual functions anywhere in `src/core/`.** Verified: `grep -rn "virtual " src/core/` returns nothing. `RomSource::page()` is a non-virtual inline header function (`rom_source.h:77`) despite the "mapped vs owned" polymorphism — that is a nice piece of design and there is no devirtualisation win to chase.
- **One `std::function` in the whole core** (`render3d.h:508`, the raster job dispatch), taken by pointer per worker per frame. Not a per-pixel indirect call.
- **`-fno-exceptions` / `-fno-rtti` are not levers.** There is exactly one `throw` in the core (`mem/page_table.h:71`, `std::bad_alloc` on allocation failure), no `try`/`catch`, no `dynamic_cast`. Zero-cost EH is genuinely zero-cost when nothing throws; `.eh_frame` is not in the icache. Adding `-fno-exceptions` would only break that one line and buy binary size.
- **LSE atomics are near-irrelevant here.** The only atomic RMWs are the raster pool's `claim()`/`remaining_` (`render3d.cpp:250, 319`, a handful per frame per worker) and two `stamp.fetch_add` in `mem/timing.cpp:133,150` which fire on region reconfiguration only. `-mcpu=cortex-a55` is worth having for its *scheduler*, not its `+lse`.
- **The 78 env knobs are not a hot-path problem.** I traced all 121 `getenv` sites. Every one in a per-frame-or-hotter file is either a namespace-scope `const bool` initialised at load time (`engine2d.cpp:445-446`, `cart.cpp:180,243`) or a function-local `static const` read once per *frame* at worst (`gpu.cpp:31` `ablate()`, `render3d.cpp:2540,2549`, both at the top of `Renderer3D::render`). None sits in a per-line or per-pixel loop. The function-local statics do carry a guard-variable acquire load per call, but at per-frame frequency that is noise. **This lead can be closed.**
- **No data race on the SPU output ring.** `Audio::push` (`src/frontend/sdl/audio.cpp:83-120`) uses `SDL_QueueAudio` with `want.callback = nullptr`, so `Spu::take` runs on the emulation thread. The plain non-atomic `rd_`/`wr_` (`spu.h:118`) are correct as written. (If anyone ever switches to a callback-based audio device, this becomes UB immediately — worth a comment in the header.)
- **Cart zip / cache paths are load-time only.** `zip.cpp`, `zip_cache.cpp` are reached from `open_zip` at startup. `RomSource::prefetch` is opt-in and documented as a measured loss (`rom_source.h:62-72`). Nothing per-frame.
- **KEY1 crypto is per *command*, not per read.** `key1_init` (`cart.cpp:142`) rebuilds the 0x412-word schedule and is called from `command_start` only on `0x3C` and on the KEY1-mode state restore — not from `rom_read32`. The brief's "KEY1/KEY2 crypto per read" concern does not apply to this implementation.
- **`src/core/bios/`** is `freebios_data.cpp` (a data blob) and `firmware_gen.cpp` (called at reset). Nothing per-frame.
- **LTO is real and covers both targets.** `CMAKE_INTERPROCEDURAL_OPTIMIZATION ON` at `CMakeLists.txt:72` is set before every `add_subdirectory`, and the ninja files confirm `-flto=auto -fno-fat-lto-objects` on core, headless and SDL alike.

---

## Bugs and fidelity-vs-speed observations

1. **`Spu::sync_state` does not save `cap_batch_`** — `spu.cpp:400`:
   ```cpp
   s.fields(cnt_, bias_, master_, muted_, mix_at_, batch_);
   ```
   `batch_` is saved but `cap_batch_` (`spu.h:114`) is not, while `ev_mix`
   (`spu.cpp:329`) selects between them. In practice both come from env vars set
   before `reset()` so a state loaded in the same process is fine — but a state
   is not self-describing here, and saving `batch_` while omitting `cap_batch_`
   is an inconsistency that will bite whenever `cap_batch_` becomes a menu
   setting. Not currently a divergence; fix it while it is free.

2. **The cheat engine can run more than once per frame, by design** —
   `cpu.cpp:158-162`. The comment justifies it ("AR codes write fixed values and
   are written to be re-run, matching the reference matters more"). That is a
   defensible fidelity call, but combined with finding 7 it means the
   full-database scan is paid an unbounded number of times per frame, not once.
   Fixing 7 removes the performance consequence without touching the fidelity
   decision.

3. **`Spu::read` calls `catch_up()` before deciding the register is write-only**
   — `spu.cpp:76-80`. Correct (the busy bits do clear as samples end), just
   noting it is unconditional. Cheap; not worth changing.

4. **`Cart::spi_flash` sector erase writes 64 KB in a synchronous loop** —
   `cart.cpp:331`: `for (u32 i = 0; i < 0x10000; ++i) sram_[...] = 0xFF;` inside
   the SPI byte handler, i.e. entirely within one emulated SPI transfer with no
   time charged. A real chip takes hundreds of milliseconds. Games that poll the
   status bit will see the erase complete instantly. This is a fidelity
   simplification in *our favour* speed-wise (a game that would busy-wait does
   not), so it is not a perf item — but it is the kind of thing that shows up as
   "the save works on hardware and not here" if a title checks the busy bit's
   *timing* rather than its value. Related work already exists
   (`io-busy-bits-by-time` did DIV/SQRT/SPI busy by `now() < ready_at`); AUXSPI
   erase looks like it was not covered.

5. **`build/asan/` is configured with `-fsanitize=address,undefined` on top of
   `-O3 -DNDEBUG` and `-flto`.** ASan + LTO is a known-fragile combination and
   `NDEBUG` disables the asserts you would want a sanitiser run to check. Not a
   perf item, but that tree is probably not testing what its name promises.

---

## Suggested order of work

1. Item 2 (`-D_FORTIFY_SOURCE=0`) and item 4 (`-fno-stack-protector`) — an hour, bit-exact, measurable together.
2. Item 1 (`-mcpu`/`-mtune=cortex-a55`) — an hour, plus one device A/B in both run orders.
3. Item 3 (PGO) — the real prize, a day or two, and the only item in this list that attacks the measured 44 % frontend stall directly.
4. Item 5a (SPU active mask) and item 7 (cheat enabled list) — an afternoon each.
5. Items 6, 8 opportunistically.

Items 1, 2 and 4 should be measured as **one** build-flags A/B rather than three,
because each is individually near or below GSDD's ~1.5 % noise floor.
