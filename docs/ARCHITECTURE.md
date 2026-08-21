# DSperate architecture

Design decisions that shape the source tree, and where each one comes from. The
*why* — measurements and design forensics of DraStic and melonDS — lives in
private research notes that are not published; this file is self-contained and
is the contract the code follows. Section *Source* lines name the note for the
maintainer's own cross-reference only.

> **Licensing.** DSperate is GPLv3. melonDS (GPLv3) is reference, oracle and,
> with attribution, a code source. DraStic is proprietary: its *techniques* were
> documented by reading the binary, and everything here is reimplemented from
> those descriptions. No disassembly, decompilation or transcription of DraStic
> appears in this tree, ever.

## 1. Tree

```
src/core/          emulator core, no I/O dependencies, builds everywhere
  types.h          fixed-width ints, clocks, frame timing
  nds.*            the system: two CPUs, bus, scheduler, GPU, SPU, I/O
  mem/page_table.* the one memory abstraction (§2)
  mem/bus.*        physical memories; keeps both page tables in sync
  mem/timing.*     region timing tables and the ARM9 PU cacheability map (§4)
  dma/             eight DMA channels (§4)
  cart/            Slot-1 retail cartridge: KEY1, secure area, save chip
  cpu/cpu.h        per-CPU context, layout is a contract with the JIT (§3)
  cpu/interp/      ARM7TDMI / ARM946E-S interpreter (always built)
  cpu/jit/         ARM -> AArch64 recompiler (AArch64 hosts only)
  sched/           event scheduler and the CPU interleave (§4)
  gpu/             2D engines + 3D geometry/rasteriser (§5)
  spu/, io/        sound; IRQ/timers/DMA/IPC/SPI/cart
src/frontend/cli/  headless runner (SDL frontend later)
tests/             ctest unit tests; differential harness later
cmake/             AArch64 cross toolchain
```

`dsperate_core` is a static library with no platform dependencies beyond
`mmap`. Frontends link it.

## 2. Memory: one tagged page table

*Source: research note "JIT design forensics" §2 (private).*

`mem::PageTable` is an array of 8-byte entries, one per **2 KB** guest page,
covering the full 32-bit address space (16 MiB, `MAP_NORESERVE`, so unmapped
pages cost nothing). The entry is a pre-biased host base shifted right by 2 with
two tag bits on top:

| bit | name | meaning |
|---|---|---|
| 63 | `CODE` | translated code lives here; stores must run the SMC check |
| 62 | `SPECIAL` | not plain RAM for writes (MMIO, ROM, protected) |

`entry << 2` recovers the pointer and drops both tags in one shift, so loads
from a code page cost the same as loads from RAM, and a pure MMIO entry (base 0)
falls to the slow path with no additional branch. `host_base + guest_addr`
addresses the byte directly because the stored base is biased by the guest
page address, so mirrors are just repeated entries pointing at the same buffer.

Consequences:

- **Every** access path — interpreter, JIT, DMA — uses this table. There is no
  second memory map.
- All mapping changes (VRAMCNT, WRAMCNT, CP15 TCM moves, cart) go through
  `mem::Bus`, which updates both CPUs' tables and preserves `CODE` tags.
- SMC detection is a **value comparison** on the store path: a write that does
  not change the byte is not a modification. Only genuine changes reach the
  block-invalidation check.
- Fastmem (melonDS-style signal-handler backed loads) is a possible later
  addition on top of this table, not a replacement for it.

## 3. CPU context is a JIT contract

*Source: research notes "JIT design forensics" §1, §5, §6 and "DraStic vs melonDS" §1–2 (private).*

`CpuContext` is standard-layout with `static_assert`ed offsets. `JitHot` groups
everything translated code touches: register homes, CPSR, the down-counting
cycle budget, IRQ-pending, the alert word, the native exit address and the
sibling CPU pointer.

The recompiler design these fields serve:

- guest `r0`–`r14` **pinned** in host registers for the whole time translated
  code runs; guest NZCV held in the host NZCV; no register allocator;
- `r15` is **never maintained** in the fast path — reconstructed from the
  native PC via per-block metadata when an interrupt/exception/SMC hit needs it;
- conditionals are one inverted `B.cc`; cycle accounting is a flag-neutral
  batched `SUB`; interrupts are polled only at block entry and after stores;
- blocks are addressed by 32-bit offsets from the cache base, linked by a bare
  `B`, with a two-instruction prologue (`tbz budget,#31` + reschedule call) and
  two entry points;
- the ARM9/ARM7 switch is a register spill/fill between the two contexts with
  no C in between.

Pinning is what makes zero-cost block linking possible: both sides of a link
agree where every guest register lives, so nothing is reconciled at the edge.
That dependency is the reason the context layout is frozen early.

The interpreter uses the same `CpuContext`, so either engine can run either CPU
and the interpreter is the JIT's fallback and first oracle (`NDS::run_arm9` /
`run_arm7` are function pointers).

**Open item:** whether the page table should be inlined into `CpuContext` so a
single base register can reach register spills (negative offsets) and page
entries (positive offsets). Currently it is a member with its own mapping;
`JitHot` is positioned so either choice stays cheap.

### 3.1 The recompiler as built (2026-08-21)

`cpu/jit/` (AArch64 only; `src/core/cpu/jit/README.md` has the file map).
What matches the design above: pinned guest registers (r0-r7, r13, r14 in
callee-saved host registers; r8-r12 in caller-saved ones the call stubs
spill), guest NZCV in host NZCV with a per-block flag-liveness pass deciding
when the `mrs/msr` merge is needed, inverted `b.cc` for conditionals, one
batched budget `sub` per straight-line run, blocks addressed by 32-bit arena
offsets in a direct-mapped LUT, lazy direct links patched into a bare `b`,
and a prologue whose only check is the budget sign.

What deliberately differs, for now:

- **Every block is exact from day one.** Anything not inlined — MMIO
  accesses, MSR, SWI, coprocessor, PC-destination ALU ops, user-bank LDM/STM,
  SWP, LDRD/STRD, the v5 DSP extensions — runs through `jit_h_fallback`, which
  executes that one instruction with the interpreter and its cycle accounting.
  Inlining is an optimisation, and every inlined form is fuzzed against the
  interpreter (§6 item 2).
- **The cycle model is the interpreter's, exactly**, including dynamic data
  costs read from the per-page table and the CD/CDI combine formulas emitted
  without flags. `DS_JIT_STRICT=1` adds a budget test after every instruction
  so the engines interleave identically (the oracle mode); normally the test
  is per block, which is the only timing difference between the two engines.
- r15 is still written to the context at every exit rather than reconstructed
  from the native PC; there is no PC-metadata table yet.
- The ARM9/ARM7 switch passes through C once per scheduler slice.
- Interrupts are taken on the C side after a poll leaves translated code.

## 4. Scheduler and timing

*Source: research note "JIT design forensics" §4–5 (private) for the budget
mechanism; melonDS for the cycle model, which our traces are compared against.*

**Clock units.** Scheduler time is a `u64` of ARM9 cycles (67 MHz). The
system clock (ARM7, timers, DMA, SPI, cart, display) is half that: a scanline
is 2130 system = 4260 ARM9 cycles, a frame 263 lines. Every event scheduled
from a system-clock device converts with `<< 1`. Getting this wrong once cost a
day: the whole machine ran at half speed relative to the CPUs.

**Interleave.** Slices of 128 ARM9 cycles (melonDS's 64 system cycles): the ARM9
runs its slice, then the ARM7 covers the same span at half clock. The ARM7's
overshoot and the odd ARM9 cycle are carried as a debt across slices; a halted
CPU consumes exactly its slice. `Scheduler::now()` includes the running CPU's
consumed budget so events scheduled from inside an instruction are stamped at
the right time. A CPU with a running DMA channel is stalled and the DMA runs in
its place.

**Cycle model** (`cpu_mem.h`, `interp.cpp`, `mem/timing.*`). Region timing
tables per 16 KB (ARM9 bus) / 32 KB (ARM7) give N/S costs for 16- and 32-bit
accesses; main RAM is a 16-bit bus with N=8/S=1, everything else 1/1, the GBA
slot per EXMEMCNT. The ARM9 adds a 3-cycle non-sequential penalty outside main
RAM and has a per-4 KB table derived from the PU region registers: cacheable
pages cost 3 cycles on a fetch at a line start or after a branch and 1
otherwise (a cache approximation, no tag state), and 3/1 for N/S data. TCM
costs 1 (the TCM windows are baked into the per-4 KB table so both engines
cost an access with one lookup). Each instruction is charged a code fetch
plus, by class, internal cycles (CI), a data access (CD) or a load (CDI),
overlapped the way melonDS overlaps them; branches pay the pipeline refill.
Each interpreter handler charges at the point melonDS does (`cpu_cycles.h`):
before the jump for ALU ops, LDR and the Thumb hi-register ops, after it for
LDM and POP, which changes the numC of the charge (new pc, state and code
region). Exceptions charge the refill of the vector jump. Measured against
melonDS on Meteos over 300 frames: 84 of 300 frames have the exact ARM9
instruction count (33 before the charge points were aligned), totals agree to
0.7% on both CPUs, and the remaining difference is the cache approximation
(melonDS takes the region cost from the jump target's page, not the fetch
address).

**DMA** (`dma/`): eight channels with immediate/VBlank/HBlank/display-start/cart
triggers, main-RAM burst unit timings, repeat and IRQ. The display-FIFO mode is
clocked by the output stage; the GX FIFO mode transfers 112 words whenever the
geometry FIFO drops below half, and a full FIFO stalls the ARM9 and its DMA
until it drains (the geometry engine keeps executing behind the stall).

## 5. Renderer: decide nothing per pixel

*Source: research note "Renderer design forensics" (private).*

- 2D is a **mask-plane deferred scanline compositor**: rasterise each layer into
  its own line buffer, build window/visibility masks, priority-encode, masked
  select, masked blend. Every stage is a straight pass over 256 pixels with no
  data-dependent branching — which is what makes NEON applicable at all.
- **Static specialisation** over runtime branching: texture wrap modes, blend
  modes, tile widths, output formats and 1x/2x scale are separate kernels chosen
  once per scanline/polygon through an indirect call.
- Fixed point 1.19.12 maps to widening multiply-accumulate + narrow-shift;
  BGR555 is shift/mask work across 8–16 lanes; interleaved pixel/attribute
  records are split by the de-interleaving load itself.
- Every NEON kernel ships next to a portable C++ reference with the same name
  and a test that diffs them. The C++ version is what non-AArch64 hosts run.
- The 3D geometry engine is fixed-point integer math and stays in software;
  the rasteriser is a scanline renderer whose spans are again straight passes
  (§5.2). The research notes do not cover DraStic's rasteriser; ours follows
  the hardware behaviour as documented by melonDS.

### 5.1 The 2D engines as built (`gpu/engine2d.*`, `gpu/gpu.*`, `gpu/vram_map.*`)

The portable C++ pipeline is in; the NEON twins come next. Per line, per
engine, in order:

1. **Planes.** Each enabled background is rasterised into its own 256-entry
   plane of 18-bit colour records plus an opacity mask (text, affine,
   extended, large-bitmap and the 3D slot on BG0). Sprites are pre-rendered
   one line ahead into an OBJ plane of palette indices/direct colours with a
   per-pixel attribute byte (priority, semi-transparent, bitmap, mosaic), plus
   the OBJ-window mask. Palette lookup is deferred to selection time, as on
   hardware (the palette can change between pre-render and display).
2. **Window plane.** One byte per pixel (BG0-3, OBJ, effects) from WIN0/WIN1/
   OBJ-window/WINOUT, with the hardware's edge-triggered activation rule in
   both axes.
3. **Priority select.** Lowest priority first, BG3..BG0 then OBJ within a
   level; each layer is a masked select that shifts the previous top record
   to "second". Produces top/second colour, layer id (laid out like BLDCNT so
   the id masks against the register) and kind (normal, semi OBJ, bitmap OBJ,
   3D).
4. **Colour effects.** Alpha blend, brightness up/down with the 3D and OBJ
   override rules, on 6-bit channels with the hardware's rounding.
5. **Output stage** (`Gpu`): display mode (graphics / VRAM / main-memory
   FIFO), master brightness, display capture into an LCDC bank, 6→8-bit
   expansion to `0xAARRGGBB` framebuffers.

Register latching follows the hardware (enables take two lines, OBJ one;
affine reference points advance per line and reload at VBlank; BG mosaic
height is latched, OBJ mosaic is live). `VramMap` gives each consumer (BG-A/B,
OBJ-A/B, extended palettes, textures, ARM7) a view of 16 KB blocks with a
direct pointer where one bank backs a block and an OR-read fallback where
banks overlap; the CPU page tables are built from the same views.

**Kernel layer** (`gpu/kernels.h`, `kernels_ref.cpp`, `kernels_neon.cpp`).
The line stages that are straight passes live as free functions over plane
pointers in `kern::ref` (portable, the behavioural definition) and
`kern::neon` (AArch64, same names and signatures); `kern::active` is a
namespace alias chosen at configure time, so the renderer has no runtime
dispatch. Current set: BG plane select, OBJ select (the OBJ plane is resolved
through the palettes once per line first), colour effects, palette
conversion, 16-colour tile rows (a 64-byte `tbl` lookup), the 3D layer copy,
master brightness and the 6→8-bit expansion. `tests/kernels_test.cpp` diffs
the twins on random planes covering every branch, and the AArch64 build's
frame dumps are compared byte for byte with the host's. The stages that are
not straight passes (tile/map fetch, sprite rasterisation, windows, the 3D
span loop) stay scalar for now; `DS_PROFILE=1` in the CLI reports where the
time goes.

### 5.2 The 3D engine as built (`gpu/gpu3d.*`, `gpu/render3d.*`)

Geometry (`Gpu3D`): a 256-entry command FIFO feeding a 4-entry pipe, with a
stall queue for writes that arrive while the FIFO is full (the ARM9 and its
DMA then sit out scheduler slices until it drains); packed GXFIFO writes and
the per-command ports; 20.12 matrix stacks (projection/texture depth 1,
position+vector depth 32); vertex colour and normal-based lighting with the
hardware's truncation points; Sutherland-Hodgman clipping in Z, Y, X order
with 64-bit interpolation and 5-bit colour quantisation between stages; the
32-bit-divider viewport transform; strips sharing unclipped vertices;
W normalisation to 16 bits and Z/W depth; the 2048-polygon / 6144-vertex
double-buffered RAM. The engine has its own clock in system cycles with
per-command costs and the vertex/polygon pipelines' overlap rules; the
scheduler advances it after every ARM9 slice and GXSTAT reads catch up first.
The GX FIFO IRQ is level-sensitive (re-raised on acknowledge while the
condition holds).

Rasteriser (`Renderer3D`): at VBlank the flushed polygon list is split into
opaque and translucent, Y-sorted (stably, bottom then top) unless manual
sorting is requested, and the render registers are latched; at line 215 the
frame is rasterised into 258x194 colour/depth/attribute buffers with a
one-pixel border and a second layer holding the pixel underneath (for
anti-aliasing and translucent blending through edges). Edges step with an
18-bit slope fraction computed as x * (1/y); attributes use the two-stage
interpolation (9-bit perspective factor along Y, 8-bit along X, then linear
by that factor, with a linear short-cut for equal W); Z interpolates linearly
with a span reciprocal, W perspective-correctly. Fill rules per edge
direction, wireframe, shadow masks/volumes via a two-line stencil, decal /
modulate / toon / highlight shading, all seven texture formats through the
texture and palette VRAM views, alpha test, equal-depth and front-over-back
depth tests, fog (with the 32-bit wrap), edge marking and AA coverage
blending follow the hardware as melonDS models it. Engine A reads the result
per line (X-scrolled by BG0HOFS) as its 3D layer, and display capture can
take it as source A.

## 6. Verification

1. Interpreter vs melonDS per-instruction trace diffs — see
   [TRACING.md](TRACING.md). Status 2026-08-20: real firmware boot (120 frames)
   and Meteos direct boot (300 frames) match with no semantic divergence on
   either CPU, 4 M distinct states each.
1b. 2D renderer vs melonDS per-pixel frame diffs (`--dump-frames`,
   `tools/compare_frames.py`). Status 2026-08-21: Meteos, Mega Man ZX,
   Bangai-O Spirits and Sonic Rush are pixel-exact over 300-400 frames apart
   from single scene-transition frames; Kirby Canvas Curse differs only in
   animation phase. AArch64 (under qemu) produces byte-identical frames.
2. JIT vs interpreter differential execution. `tests/jit_test.cpp`
   (cross-built, runs under qemu) fuzzes random straight-line ARM and Thumb
   sequences on both CPUs through both engines and compares registers,
   flags, consumed cycles and memory, shrinking a failure to the shortest
   failing prefix. On whole games, `DS_JIT_STRICT=1 DS_DEBUG_SLICES=1`
   makes every scheduler slice comparable line by line (`--interp` vs
   default) and frame dumps must be byte-identical. Status 2026-08-21: the
   fuzzer found CLZ inlined on the ARM7 (undefined there), unaligned Thumb
   LDM bases, the ARM7 multiply carry, register shifts by multiples of 32
   above 32, and that an instruction routed to the interpreter observes
   every flag (liveness must treat it as a read of all four); the slice
   diff found that the interpreter charged no code-fetch cycles for ARM9
   Thumb code (a double parity test), which is what led to aligning the
   charge points with melonDS. After that: 300 random sequences per
   CPU/state pass, and Meteos, Kirby Canvas Curse, Super Mario 64 DS (120
   frames) and Golden Sun (150) run in exact lockstep — every scheduler
   slice identical, frames byte-identical.
3. NEON kernel vs C++ reference, vector by vector.
4. Performance against the measured baseline: DraStic holds SM64DS at 100% on
   **37.2% of one Cortex-A55 @ ~1.58 GHz** (RK3566). That is the number.

## 7. Targets

| tier | device | role |
|---|---|---|
| low | Anbernic H700 | regression |
| mid | RK3566 | **the benchmark device** (only one carrying both DraStic and melonDS) |
| upper-mid | T618 | regression |
| high | Retroid Pocket Mini V2 (SD865) | regression |

Development on x86 runs the interpreter and C++ kernels; `cmake --preset
aarch64-cross` + `qemu-aarch64-static` exercises the JIT/NEON paths before a
device does.
