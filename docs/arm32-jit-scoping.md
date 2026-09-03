# Scoping: an ARM → ARMv7 (A32) recompiler

Status: scoping only, 2026-09-02. Nothing here is built. Branch context:
`arm32-build` already gives an ARMv7 host tier (NEON subset kernels,
interpreter only); the JIT is the one thing that tier lacks.

## 1. Verdict in one paragraph

This is a new backend, not a port. The A64 JIT's speed comes from a static
register map that pins 20 values (15 guest registers, budget, page table,
timing table, arena base, context) in 31 host registers; A32 has 13 usable
registers, so the central design decision of `docs/techniques/01` does not
transfer and must be replaced by a per-block register cache. What *does*
transfer, unchanged, is about 70 % of the recompiler by line count: the block
cache, LUT, SMC tracking, park-and-revive, timing dependencies, the
pre-translation worker, the helpers, the fallback policy, the flag-liveness
pass, the cycle accounting, the hot/cold layout and the whole verification
story (fuzzer + scene hashes). And A32 gives back two things A64 lacks:
predicated execution and a shifter operand whose carry-out is the guest's,
so guest data-processing instructions become *one* host instruction in more
cases than on A64, including register-specified shifts and conditional
instructions. Expect roughly 3–3.5 k new lines, two to three weeks of
sessions to an exact and device-measured backend, with the register cache
and the flag-clobber discipline as the two genuinely new risks.

There is no prior art inside melonDS: it ships `ARMJIT_A64` and `ARMJIT_x64`
only. Its `ARMJIT_RegisterCache.h` is host-generic and GPL-reusable with
attribution (our README already permits this).

## 2. What the current JIT is, by portability

| piece | lines | verdict |
|---|---|---|
| `emit.h` A64 encoder | 361 | replace: new `emit_a32.h`, same style (subset of forms, objdump-verified) |
| `runtime.cpp` stub emission (`emit_stubs`, ~400 lines) | ~400 | replace: same stubs, A32 encodings, different register convention |
| `runtime.cpp` everything else (cache, LUT insert, SMC, park/revive, pretx worker, `jit_h_*`, attach/report) | ~1100 | keep, split into a shared file; a few pointer-width fixes |
| `translate.cpp` block driver, liveness, `*_needs_fallback`, `*_ends_block`, cycle accounting (`numC`, `add_pending`, deps), hot/cold splice | ~600 | keep as-is or share |
| `translate.cpp` per-instruction emission (data processing, shifter, memory, LDM/STM, branches, multiplies, MSR) | ~1150 | rewrite; the *structure* copies, the emitted shape changes |
| `jit_internal.h` register convention + context offsets | ~60 | replace |
| `jit_internal.h` Block/Runtime/LUT/JitCpu | ~230 | keep; `entry_words`, `JitCpuHot` offsets become width-aware |
| `tests/jit_test.cpp` | 384 | keep; it is engine-agnostic (interp vs JIT on random sequences), runs under `qemu-arm-static` |

Recommended layout: `src/core/cpu/jit/` keeps the public `jit.h`; the
host-agnostic runtime moves to `runtime_common.cpp`; `a64/` and `a32/`
subdirectories each hold `emit.h`, `stubs.cpp`, `translate.cpp`,
`convention.h`. CMake picks one by `DSPERATE_HOST_AARCH64` / `_ARM32`. Do not
try a shared translator with a virtual backend: the A32 translation is
shape-different (predication, native flags, register cache) and an
abstraction that fits both would be worse than either.

## 3. The host machine, and why the design changes

**Registers.** r0–r3 and r12 are caller-saved scratch, r4–r11 callee-saved,
sp/lr/pc reserved. Thirteen usable against 31. The A64 map pins:

| A64 | value | A32 answer |
|---|---|---|
| x19–x28, x9–x13 | guest r0–r14 | per-block register cache (§4) |
| w8 | cycle budget | pin (callee-saved); tested by sign at every block entry |
| x29 | `CpuContext*` | pin (callee-saved) |
| x14 | page-table base | pin (callee-saved); hot in every memory access |
| x15 | timing-table base | load from the context when needed, or a per-block literal; not pinned |
| x18 | arena base (LUT probe) | `movw/movt` absolute in the stubs; not pinned |
| x16/x17 | helper address / flag temp | r12 + `movw/movt` |

That leaves five callee-saved registers (r4–r8 say) as the cache's
long-lived slots and r0–r3, r12 as scratch and short-lived slots. Ari64's
ARM dynarec runs a full MIPS guest on this same budget with four pinned
registers, so it is workable, but the memory fast path alone wants four
temporaries (address, entry, biased base, cost) and the ARM7 cost model
another two, so the scratch set is spoken for. Whether the page-table base
stays pinned is a phase-3 knob: as a per-block literal it costs one `ldr`
per block, not per access, if the cache keeps it in a scratch across the
block.

**Flags.** Guest NZCV live in host APSR, as on A64, and this is where A32 is
*better* than A64, not worse:

- `ands/eors/orrs/bics/movs/mvns` with an immediate-shift or register-shift
  operand set N, Z and the shifter carry-out exactly as the guest does, and
  leave V. The A64 backend's `merge_keep_cv` / `merge_set_c` stubs, the
  `shift_reg` 64-bit-widening sequence (~10 instructions for a register
  shift with carry) and the RRX `cset/extr` pair all collapse to one
  instruction. The register-shift rules for amounts ≥ 32 are architecturally
  identical from ARMv4 to ARMv7.
- `smulls/umulls/smlals/umlals` set N and Z from the 64-bit result natively;
  the inline `mrs/ubfx/bfi/msr` merge for long multiplies goes away.
- Every guest conditional instruction is one predicated host instruction.
  The `use_csel_op` / `dp_csel_` special case and the branch-around form
  both disappear; `charged_ahead_` (charge before the condition test) stays.
- QADD/QSUB/QDADD/QDSUB and the SMLAxy family are native (v5TE ⊂ v7); the Q
  flag is the one wrinkle (it is sticky in host APSR and must be merged into
  the memory CPSR at sync points; keep the Q-setting forms as fallbacks in
  phase 2, inline them in phase 4).

The price: A32 in ARM state has no flag-neutral compare-and-branch (`cbz`,
`cbnz`, `tbz` are A64/Thumb-only). Every test in the memory fast path
(`lsls r3, r2, #2 ; beq slow`, `tst r2, #0xC0000000 ; bne slow`), the budget
test and the LUT tag compare clobbers NZCV. The liveness pass already knows
whether NZCV are live after each instruction; when they are, the fast path
brackets itself with `mrs r12, apsr … msr apsr_nzcvq, r12`. How often that
fires is a phase-0 measurement on the A64 side (§6). The budget test at block
entry is `cmp`-free: `tst rbudget, #0x80000000` clobbers too, so the entry
must save flags only when the *previous* block left them live across the
link — which the liveness pass can also tell at link time (a block whose first
instruction does not read flags needs no save). Alternative: keep
`budget - 1` and use `adds rbudget, rbudget, #0` only when flags are dead,
`mrs`-bracket otherwise; or `teq`-free forms via `movs`. This is the one
place to spend design time before coding.

**Encoding choice.** Emit A32 (ARM state), not Thumb-2. Fixed 4-byte words
keep `patch_rel`, the 12-byte `kill_block` entry patch, literal-argument
stubs and the hot/cold fixups as they are; predication needs no IT-block
bookkeeping; `bl` + `ldr r0, [lr]` reads a literal argument exactly as the
A64 stubs read through x30. Branch range is ±32 MB (A64: ±128 MB): the arena
is 64 MB today (`ARENA_BYTES`), so either halve it for the A32 tier or give
far links an `ldr pc, [pc, #-4]; .word` form. C helpers are Thumb-2 on
Debian armhf: call them with `blx rX` after `movw/movt`, never `bl`.

**Guest ⊂ host.** ARMv4T/ARMv5TE data-processing, multiply, CLZ and DSP
encodings are valid ARMv7 encodings. When rd/rn/rm/rs all sit in cache
slots, the host instruction is the guest word with four register fields
substituted; the `(instr >> 16 & 0xf) + 0xd` idiom the technique doc found 52
times in DraStic's A64 translator is most plausibly inherited from exactly
this trick in its 32-bit ancestor. That is the density lever A32 has and A64
does not. Guest quirks the host does *not* share and that the translator
keeps: ARM7 unaligned `LDR` rotate and `LDRH`/`LDRSH` rotate/extend, ARM7
multiply destroying C, PC-relative operands (`pc + 8/12` materialised),
`S`-with-rd=pc (fallback), user-bank LDM/STM (fallback), SWP/LDRD/STRD
(fallback, as today), mode-changing MSR (fallback).

## 4. The register cache

The one component with no counterpart in the A64 backend. Contract:

- A block starts with every guest register in memory (`JitHot::regs`, off
  the pinned context) and NZCV in APSR. First read of a guest register
  allocates a slot and loads it; first write allocates without loading and
  marks the slot dirty. At the block end, before any `bl` to a full-spill
  stub, and before a fallback, dirty slots are written back. Slots are
  callee-saved (survive `call_pure` helpers) or caller-saved (spilled by the
  stubs, as r8–r12 are today).
- Eviction is LRU-by-next-use within the block: the translator already has
  the whole block's instruction list (`instrs_`) before emitting, so a
  backward pre-pass can compute next-use distances the way the liveness pass
  computes flag liveness. That makes the allocator deterministic, which
  matters for park-and-revive (a translation must be a pure function of key,
  guest bytes, timing stamp and CPU, and it stays one).
- Linked blocks no longer "reconcile nothing at the edge": every link pays
  the caller's write-backs plus the callee's loads. With a median block of a
  handful of instructions this is the structural cost of the A32 backend and
  the thing to measure first (§6, block-length and register-use census).
  Mitigation later, if it shows: a fixed *pinned subset* (the two or three
  guest registers that the census says carry most of the traffic, plausibly
  sp and one or two of r0–r3) that never leaves its slot and that every block
  agrees on, DraStic/gpSP style, with the cache for the rest. Decide the
  subset from the census, not by guess.
- LDM/STM with n registers in cache: load or store directly to slots for
  the cached ones, spill others through memory. Fast-path rule (one 2 KB
  page) unchanged.

`melonDS/src/ARMJIT_RegisterCache.h` implements this allocator generically
(host-agnostic, templated on the compiler); PPSSPP's `ArmRegCache` is the
same idea specialised to ARMv7. Either is a day's reading and a legitimate
starting point under our licence policy, with attribution.

## 5. Everything else that changes

- **`PageTable::Entry`** becomes `u32` on 32-bit hosts: tags in bits 31/30,
  `base = e << 2` still exact (biased base = host − guest page, 2 KB aligned,
  30 bits). The interpreter and all `read_ptr`/`write_ptr` users are already
  width-agnostic; the assert in `make_entry` and `BASE_MASK` need the width.
  The table halves to 512 KB, which the interpreter tier also gains.
- **`JitCpuHot` offsets** (`OFF_JC_PT/TIM/ARENA` = 0/8/16) assume 8-byte
  pointers: derive from `sizeof(void*)`. `JitHot::exit_native`/`other_cpu`
  stay `u64` (emit a 32-bit load at the low word).
- **`Block::entry_words[3]`** and the 12-byte kill patch: A32 redirect is
  `ldr pc, [pc, #-4] ; .word dispatch` (8 bytes); keep 12 for one layout.
- **LUT entry** `(offset << 32) | key` is 64-bit: `ldrd` needs an 8-byte
  aligned base with no shifted index (`add r2, rlut, r2, lsl #3 ; ldrd r2, r3,
  [r2]`), then `cmp r2, key` — flags, so the dispatch stub saves APSR as
  `branch_indirect` already does with x17. Or split into two u32 tables; a
  phase-3 A/B. LUT sizing (64 K entries won on the A55 against the
  technique's 1 K) must be re-measured on the 32-bit device: a different
  cache hierarchy, and the README's argument was device-specific.
- **`sync_icache`**: `__builtin___clear_cache` is the `cacheflush` syscall on
  ARM Linux; works under `qemu-arm` too. W^X, `mmap` of the arena, and the
  pre-translation worker's staged blocks are unchanged.
- **Stubs** (all ~400 lines, one file): `enter`/`enter_light`/`run_loop`
  (`push {r4-r11, lr}`), `exit_key`, `call_pure`/`call_full`/`call2`, `poll`,
  `slow_load/store[3]`, `dispatch`, `link`, `fallback`, `branch_indirect`
  (+`_cdi`), `flush_exit`. The flag-merge stubs are not needed. The
  ARM9 refill-cost sequence in `branch_indirect_cdi` uses `csel` heavily:
  predicated `mov` replaces it one for one.
- **Cycle model** emission maps directly: `bic rt, rt, rt, asr #31` for
  flagless max, `mla` for `madd`, `ubfx/bfi` exist (v6T2+), `sub` immediates
  cover the budget subtracts (`sub_imm_any` may need a `movw` pair for
  pending counts above 255 that are not rotatable — rare).
- **64-bit instrumentation** (`DensitySlot::execs`) becomes `adds/adc`;
  measurement only.

## 6. Phase 0: measure before designing (A64 side, 1–2 days)

Everything below is cheap instrumentation in the *existing* translator, run
on the five replay scenes under qemu or the rig, and each answer changes a
design choice above:

1. **Executed-weighted guest-register use** per block (reads, writes, and
   which registers cross block boundaries live). Decides the pinned subset
   and the callee-/caller-saved split of cache slots.
2. **NZCV liveness at every memory access and at every block entry**
   (`live_` is already computed). Decides how much `mrs/msr` bracketing the
   fast path will carry; if it is above ~20 % of accesses, design the
   flag-neutral form (§3) before phase 2 instead of after.
3. **Block length and link-edge distribution** (guest instructions per
   block, entries per block; `DS_JIT_DENSITY` already counts entries).
   Sizes the register-cache boundary traffic.
4. **Fallback census by opcode class** (`DS_JIT_HIST`), to order phase 2.
5. **Interpreter-only baseline on the Miyoo A30** (§9), from the
   `arm32-build` branch, with the same five scenes: mean, p99 and
   over-budget frames, plus the RSS. The A55 numbers put the interpreter at
   51 ms/frame and the JIT at ~15 on SM64DS; a Cortex-A7 at 1.2 GHz is
   ~2.5–3× slower per thread, so the JIT alone does not reach 60 fps there
   and the renderer (6–11× the JIT's share on the A55) still dominates. The
   baseline says how far off we start and whether `perf` works on the
   device's kernel (§9).

## 7. Phases 1–4

**Phase 1 — exact skeleton (≈ 3–4 days).** Runtime split; `u32` page-table
entry; `emit_a32.h` verified against `arm-linux-gnueabihf-objdump` the way
`emit.h` was; stubs; a translator that emits every instruction as a
fallback, then straight-line blocks with the budget test and dispatch.
Gates, all under `qemu-arm-static` on the dev box: `test_jit`; `tools/
scene_hashes.sh` equal to the interpreter under `DS_JIT_STRICT=1`; and,
non-strict, equal to the **A64 JIT's** hashes — since block boundaries,
budget-check points and the cycle model are the same, the A32 backend must
reproduce the A64 backend's block-granularity overshoot exactly. That is a
stronger oracle than the interpreter alone (etody/dbori drift between
interp and JIT is expected and documented; between the two JITs it is a
bug).

**Phase 2 — inline the common case (≈ 1 week).** Register cache; data
processing by field substitution with native flags and predication; memory
fast path with the flag bracket; LDM/STM; static/indirect branches; multiply
and DSP; same-mode MSR; CP15 no-ops. Fuzzer after every step; `DS_JIT_
FASTCOST=1` must still make it fail (mutation check).

**Phase 3 — device tuning (≈ 1 week, needs the device).** Pinned subset;
page-table base pinned vs literal; LUT size; A/B by PMU categories with
p99 and over-budget frames reported, both run orders, `--quantum 0`.

**Phase 4 — optional.** Q flag inline; Thumb-2 emission for density (only
if I-fetch stalls dominate as they do on the A55); fastmem. Fastmem was
ruled out on A64 on cost grounds (0.52 ms walk), but on A32 its motive is
different — it removes the flag-clobbering tests and three temporaries from
every access — and a 256 MB low-guest-space reservation fits a 3 GB
32-bit process. Still expensive (SIGSEGV back-patching, ~5 k MMIO sites a
frame); do not start there.

## 8. Risks and open questions

- **The device is a Miyoo A30** (§9); the ARM32 kernel tier has never
  been measured on it, and neither has the interpreter.
- **Register-cache boundary traffic** could eat much of the A32 density win
  on short blocks; the census says how much before any code is written.
- **Flag save/restore around memory accesses** is the second unknown, same
  remedy.
- **qemu-arm as the only gate** until a device exists: 900-frame scene runs
  are slow but were done for the A64 subset gate already; qemu-arm's TCG is
  fine for predication and flags but the LineWorker Dekker pair note
  (`stlr/ldar` ordering) applies to any qemu-user run — keep `DS_WATCHDOG`
  handy. 64-bit atomics on ARMv7 go through `ldrexd/strexd`; check the
  worker hand-off compiles without a libatomic lock.
- **Toolchain**: `arm-linux-gnueabihf-g++-13` and `qemu-arm-static` are on
  the dev box and an armhf headless binary links today; a `test_jit` run
  under qemu-arm has not been tried (the target is gated on `DSPERATE_JIT`,
  which CMake turns off for non-AArch64 hosts).
- **Reading list before phase 1**: gpSP's ARM backend (Exophase; the
  32-bit ancestor of the DraStic technique), Ari64's `assem_arm.c` in
  PCSX-ReARMed (register allocation on ARMv7), `ARMJIT_RegisterCache.h` in
  melonDS, Dolphin/PPSSPP `ArmEmitter.h` (a complete A32 encoder; ours will
  be a subset written from the ARM ARM to match `emit.h`'s style and its
  objdump verification).

## 9. Target: Miyoo A30 (Allwinner A33)

What the SoC is, and what each fact changes. Verify the kernel version on the unit before phase 0.

- **Four Cortex-A7 at ~1.2 GHz, ARMv7-A, NEON + VFPv4.** Exactly the
  `-mfpu=neon-vfpv4 -mfloat-abi=hard` tier already built; add
  `-mcpu=cortex-a7`. In-order, partial dual-issue, 8-stage pipeline: the
  A55 stall-bound finding transfers (predicated instructions are free,
  `mrs/msr apsr` are a few cycles each, not serialising), but the branch
  predictor is weaker and indirect prediction shallow, so dispatch and the
  LUT probe weigh more than they did on the A55. Four cores match
  `THREADS=3` with no core left for the display server, audio and SDL;
  phase 3 must re-run the worker-count knob here rather than trust the
  RK3566 pin, and [[gsdd-noise-floor]]'s "2 workers beat 3" is the prior.
- **32 KB L1I / 32 KB L1D per core, 512 KB shared L2.** Same L2 as the
  RK3566 but half the L1 of the A55 (64 KB L1I there): the I-fetch share of
  the stall picture gets worse, which is the case for the field-substitution
  density win and, later, for Thumb-2 emission (§7 phase 4).
- **512 MB DDR3, with a self-imposed ~256 MB process budget.** The device
  has 512 MB; the firmware, its display stack and file caches take their
  share, so the emulator should hold itself to about 256 MB RSS. The A64
  tier's JIT footprint is 64 MB arena, 1 MB of LUTs, two page tables (512 KB
  each as `u32`), an 8 MB ARM9 timing table, plus the texture cache and three
  workers' band buffers. The likely largest single item is the ROM: it is
  loaded whole today (a 128–512 MB cart on its own breaks the budget), so a
  cart-streaming switch (page in from the SD card on demand, with the
  KEY1/KEY2 read path unchanged) is on the list for this tier — not JIT
  work, but the same session should measure it. Halve the arena to 32 MB
  (which also keeps every `b` in range, §3) and record RSS in the phase-0
  baseline; the 64 K-entry LUT stays if the budget allows.
- **640×480 IPS, mounted portrait (SDL reports 480×640; the spruce
  launcher passes `DS_ROTATE=270`).** Measured 2026-09-03 on the firmware
  menu: the device SDL2 (2.26.1, Allwinner Tina SDK) has one real video
  driver, `mali`: EGL over `/dev/fb0` through the Mali-400 r5p1 blob. No
  software fbdev driver, so SDL's window surface is a hidden GLES upload
  (the KMSDRM trap). Software renderer present: 97 ms; `--accel` (GLES2
  renderer): 3 ms, vsync free. Rotation in the spruce fork
  (`patches/0002-screen-rotation.py`) is renderer-only: draw into a
  logical-size render target, one rotated `SDL_RenderCopyEx`, geometry
  swapped in `out_size()` and undone in `map_point()`; it turns scanline
  scaling off under rotation.
- **Why the scanline path does not win here (measured, `fbprobe.c` /
  `rotprobe.c`).** fb0 is 480×640 ARGB, triple-buffered (virtual 480×1920),
  and its mmap is uncached; `FBIOPAN_DISPLAY` blocks for vsync (16.5 ms),
  `FBIO_WAITFORVSYNC` is a no-op. A 1.2 MB bulk 64-byte-store copy into it
  costs 1.7 ms, but any small-store write pattern costs 5–8 ms (row-wise
  1920 B memcpys 7.9 ms; the scale kernels' 16 B stores would land in that
  regime), and a proper NEON 16×4-transpose rotate-copy with 64 B column
  stores costs **4.7 ms** into the fb and 5.2 ms into cached memory -- the
  transpose is load-latency bound on the A7, not fb-bound. So a rotated
  scanline tier is: scale into cached staging (~1–1.5 ms) + rotate/copy
  (≥1.7 ms unrotated, 4.7 rotated), i.e. no better than the 3 ms Mali
  present, before counting that the core would still rotate somewhere.
  **Verdict: on the A30 the GLES renderer with the fork's RenderCopyEx
  rotation is the right present path; the scanline tier stays off there.**
  Upstreaming DS_ROTATE (the fork's shape) and defaulting `--accel` on the
  ARM32 tier is what to land.
- **The display engine's scaler layer works (measured 2026-09-03,
  `scalertest.c`).** The kernel is Allwinner "disp 1.5"; the ABI is
  libvdpau-sunxi's `kernel-headers/drv_display.h` (the sysroot's
  `drv_display_sun4i.h` is the wrong generation): `ioctl(/dev/disp, cmd,
  ulong[4]{screen, layer, &disp_layer_info})`, LAYER_ENABLE/DISABLE/
  SET_INFO/GET_INFO = 0x40..0x43, `disp_layer_info` 108 bytes, verified
  byte-for-byte against fb0's own layer (layer 3, pipe 1, phys
  0x58200000). Layers 0–2 are free. Setting layer 0 to SCALER mode with a
  192×256 ARGB source (`src_win`) and a 480×640 `screen_win` is accepted,
  reads back as such, and the sysfs dump shows it. Per-frame CPU cost:
  repaint of the 192×256 source 0.36 ms, address flip via SET_INFO
  0.05 ms. Visually confirmed on the unit: `/mnt/SDCARD/dsperate/scalertest 0 1 1`
  fills the panel with the scaled checkerboard, the red border sits on the
  screen edge and the diagonal spans the full width and 3/4 of the height
  (192/256), i.e. the scale factors are exactly the expected 2.5 × 2.5.
  Design that follows ("DispOut" tier): SDL on the dummy video driver for
  input/audio; the core renders 256×192 per screen as on the renderer
  path; two NEON transposes into a 384×256 physically contiguous source
  (fb0's spare buffers while the emulator owns the panel) ≈ 0.7–1 ms; one
  scaler layer with `screen_win {0,160,480,320}`; flip by SET_INFO, vsync
  by fb0 pan. Estimated present ≈ 1 ms of CPU and no GL driver threads,
  against 3 ms for `--accel`. Trade-off: the DE scaler filters (smooth
  scaling); the LCD grid, chunky and seam modes are CPU scanline features
  and would not apply on this tier.
- **Kernel 3.4.39, glibc 2.23, busybox, no `perf`.** Confirmed on the
  unit. The build that runs there is the spruce recipe: steward-fu's A30
  buildroot toolchain (GCC 13.2, glibc 2.23 sysroot, SDL2 2.28.5 headers),
  `-mcpu=cortex-a7 -static-libstdc++ -static-libgcc`, glibc floor 2.17 in
  the result. The dev box's own armhf cross g++ targets glibc 2.39 and its
  binaries will not start on the device. The toolchain, a CMake toolchain
  file and the built binaries live in this session's scratchpad; the
  A32-subset NEON kernels build and pass all 13 tests under qemu-arm.
- **Governor.** spruce locks the cpufreq and hotplug sysfs files to 0444
  (`cpu_control_functions.sh`: `unlock_governor`/`lock_governor`,
  `cores_online`), and at idle only cpu0 and cpu3 are up under
  `conservative`. spruce's Performance mode is 1344 MHz on all four cores and
  its Overclock 1512 MHz (`A30.cfg`); the baseline below ran with the
  governor at `performance` but `scaling_max_freq` still capped at 1008 MHz. A benchmark script must chmod and set these itself
  and restore them after.
- **First baseline (headless firmware boot, 300 frames, interpreter,
  A32-subset NEON).** Median 80.7 ms, p99 181.7 ms at 1008 MHz on four
  cores (expect ~25 % better at spruce's 1344 MHz Performance cap); the conservative governor gave 84.3, so it ramps under load and
  the cap is what matters. That is about 3× the A55 interpreter figure, as
  predicted. The JIT has to buy ~4–5× on this scene before the renderer
  question even arises.
- **Access.** SD card, and ssh/adb only under some custom firmwares: set up
  the rig-style key access ([[rig-device-access]]) before phase 3 so A/Bs
  are scriptable.
