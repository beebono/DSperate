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
  spu/, io/        sound (§6); IRQ/timers/DMA/IPC/SPI/cart
src/frontend/cli/  headless runner: traces, frame and audio dumps (§7)
src/frontend/sdl/  the playable frontend: screens, sound, input (§8)
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
  VRAMCNT changes are applied as a diff (`PageTable::remap`: the desired
  host pointer per 2 KB page for the whole 16 MB region, then only the
  entries that change are touched). Mario & Luigi rewrites VRAMCNT ~4.6
  times per frame (bank swaps for capture); unmapping and remapping 8 K
  pages per CPU each time, with a code-page lookup per page, was 6 % of the
  game (2026-08-22: −3.4 % on the device after the diff).
- SMC detection is a **value comparison** on the store path: a write that does
  not change the byte is not a modification. Only genuine changes reach the
  block-invalidation check.
- Fastmem (melonDS-style signal-handler backed loads) is a possible later
  addition on top of this table, not a replacement for it.

### 2.1 Why not host-MMU fastmem

The traditional trick — reserve 4 GB of host address space, `mmap` the guest
memories into it, let the MMU translate and a SIGSEGV handler catch I/O —
would replace four instructions of our emitted fast path (page index, entry
load, base recovery, unmapped test) with one `ldr`. It cannot touch the other
half of that path: cycle accuracy needs the *region* of every access to
charge the right N/S cost, so the timing-table load and the charge stay
regardless. Emulators where fastmem transforms things do not pay per-access
timing; we do, and the trace comparison against melonDS rests on it.

The DS then charges extra for the privilege: VRAMCNT/WRAMCNT bank switching
becomes `mmap`/`mremap` with TLB shootdowns where a table rewrite does now
(and games rebank mid-frame); SMC detection becomes `mprotect` plus write
faults instead of a tag bit that `lsl #2` discards for free; every I/O
register poll becomes a signal, microseconds each, unless the recompiler can
prove RAM-ness statically. Two CPUs with different maps and the TCM overlays
sit on top of all that. Measured against a profile where translated code is
6-11 % of the process and the 3D rasteriser is 31-46 %, the ceiling is ~1-3 %.

The cheap version of the same idea, if the memory path ever does become hot:
the two lookups hit two tables and therefore two cache lines, while the page
entry has ~14 spare bits between the 48-bit biased base and the two tags.
Either pack the four cost bytes into those, or interleave the tables at a
16-byte stride and fetch both with one `ldp` (`add x2, x14, x2, lsl #4` +
`ldp x2, x6, [x2]`): three instructions instead of five, one cache line
instead of two. The cost is coupling the timing granularity (4 KB on the
ARM9, 32 KB on the ARM7) to the 2 KB page entries, so every remap writes
both fields.

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
  from the native PC; there is no PC-metadata table yet. Exits are cold code
  (`bl exit_key_lit; .word key`), so nothing is materialised on a hot path.
- The ARM9/ARM7 switch passes through C once per scheduler slice.
- Interrupts are taken on the C side after a poll leaves translated code.

Layout and calling conventions as of the second pass (2026-08-21 p.m., see
`cpu/jit/README.md`): each block is a hot section followed by a cold section
spliced in at the end of translation; stubs read literal arguments from the
words after their `bl` through x30; the budget register holds `budget - 1`
so every test is one `tbnz`; memory slow paths are pure calls into the
interpreter's `mem_read*/mem_write*` logic rather than re-executions of the
instruction. Measured on the RK3566, the translated code is ~14 % of a
SM64DS frame; the `perf` profile (the A55 PMU is usable on the device)
drives the order of the remaining work.

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

**Idle skip (2026-08-22).** When both CPUs are halted with no unmasked IRQ
pending, no DMA running and the geometry engine idle, nothing can happen
before the next event, so the slice runs to the deadline instead of the
quantum (`Scheduler::both_idle`). Exact: the ARM7 debt's parity carries the
same way, `run_to` only stamps the time. On SM64DS and Meteos 63 % of all
slices were of that kind (31 % on Mario & Luigi); the count halves, wall
time drops only 1.3–1.7 % — an idle slice was already cheap.
`DS_PROFILE=1` prints the slice breakdown (halted per CPU, both, with DMA,
run to the deadline).

**In-place refill — tried and reverted (2026-08-22).** The next idea was
to keep the ARM9 in translated code across slice boundaries: when its
budget runs out at the block-prologue check or a poll, a helper replays
the slice end and start (GX catch-up, ARM7 debt, next capped budget) if
the ARM7 is asleep, no DMA runs, nothing is pending and no event is due,
and the block simply continues. It is exact (byte-identical JIT frame
dumps on the device and under qemu) and it fired for 1.5 M of SM64DS's 2.1
M ARM9 slice ends — and wall time did not move (15.90 → 15.82 s per 900
frames). Leaving and re-entering translated code was never the cost; the
scheduler's ~6 % in profiles is the cache-cold scheduler and context state
touched at every boundary, and whoever touches it first pays. What does
move it is fewer boundaries: `DS_QUANTUM=512` ran the SM64DS scene 2.5 %
faster (91.7 → 89.4 s), but 1024 wedged SM64DS outright.

**The 1024 wedge (2026-08-22) was ours, not the game's.** `fire_due` made
one pass over the event table. An ARM7 timer whose period is shorter than
a slice reschedules itself, from its own handler, at a time the CPUs have
already overshot; the single pass never revisits it, `run_frame` then
calls `run_until(next_deadline())` with `now_` already past that deadline,
the loop body never runs and nothing ever fires again — the emulator spins
with the frame counter stuck. `fire_due` now repeats its pass while
anything is due, and `run_until` fires due events on entry. Byte-identical
at 128 (the state could only arise by hanging); SM64DS runs at 1024 and
4096. The quantum question is then purely a timing-fidelity trade: the
interleave decides how far one CPU can get ahead of the other before it
sees the other's IPC writes and IRQs, and 128 (melonDS's 64 system
cycles) keeps the trace comparison in lockstep.

**Event-bound interleave (2026-08-22).** DraStic has no quantum: its slice
is the distance to the next pending event (private research notes,
`slice_cycles = events.pending`), so one CPU may be a whole event interval
ahead of the other — which is where its <1 % scheduler comes from. We now
offer the same: `Scheduler::set_quantum(0)` bounds slices only by events
(scanline, HBlank, timers, SPU: one to a few thousand cycles). Device,
no-input 1800 frames: SM64DS 33.2 → 32.0 s (−3.6 %); the replayed scenes
at 512 (the rows whose work stayed comparable — input replays diverge
across quanta, so scene times can't be compared beyond that) −2.4 % SM64DS,
−4.1 % Mario & Luigi, −4.5 % Meteos. The SDL frontend runs event-bound by
default (`--lockstep` for 128); the CLI stays in lockstep by default,
because every frame baseline and trace comparison assumes it.
Event-bound slices exposed a gap in the geometry FIFO model: the engine
only catches up at slice ends, and within a slice the ARM9 (or the FIFO
DMA) could push unboundedly into a full FIFO — a 64-entry stall queue
absorbed that at 128 cycles but overflowed at event length, dropping
commands (missing geometry in SM64DS's intro). In event-bound mode the
ARM9 now stops the moment the FIFO fills (`Scheduler::gx_fifo_full`, the
DMA-preempt mechanism), sits out after a DMA step left it stalled, and
re-checks every 128 cycles while stalled; lockstep keeps the queue
behaviour, which melonDS's timing assumes.

**Native slice loop (2026-08-22).** With the recompiler attached,
`Scheduler::run_until` hands the slice sequence to a loop in the code arena
(`jit::run_loop`): it saves the callee-saved registers once, then alternates
`Scheduler::slice_next` — run_until/run_cpu/jit::run written as one
straight-line sequence per slice with two resume points, returning the
context and native entry to run next — with `enter_light`, which loads the
guest state and jumps; the exit stubs `ret` straight back into the loop.
Interpreted CPUs and DMA run inside `slice_next`, so all engine mixes use
it. The sequence of operations is identical to the C++ loop (kept for x86),
and the check is byte-equal JIT frame dumps against the previous build on
six games plus the strict slice diff. Measured: 2–6 % of wall time (Mario
& Luigi 4.37 → 4.16 s per 400 frames); the first version as a `switch`
state machine was *slower* than the C++ loop — the in-order core
mispredicts jump tables and indirect calls — and a function-local static
costs an acquire load per use. What remains per slice is the guest
register save/restore on each side and ~100 instructions of slice logic;
the next step would refill the running CPU's budget in place (a pure call
from the budget poll) while the other CPU is halted and nothing is due.

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

**2D pass (2026-08-21 p.m.), from the device profile.** The first NEON
kernels were straight translations and the profile on the A55 showed where
that was wrong: `composite_line` computed all three blends for every pixel
(43 cycles/pixel); `draw_bg_text` fetched map entries and tiles through the
out-of-line VRAM accessors and resolved 256-colour pixels one at a time;
palettes were reconverted per background per line; every enabled plane was
selected even when empty; the output stage ran three passes over each line.
Now: the effects kernel tests each 16-pixel block for "nothing blends" (one
`umaxv`) and copies through, gating each blend kind by block otherwise;
text backgrounds gather the 33 tile rows of the line through direct VRAM
pointers (one 64-byte map run per screen block) and one kernel
(`text_tiles_16` / `text_tiles_256`) resolves the whole row at the scroll
offset into a padded plane — no per-tile call, no copy; palette conversions
(standard and extended, by slot and number) persist across lines and are
reused while a `memcmp` against the copy taken at conversion time still
matches; planes report whether they have any opaque pixel and empty ones are
skipped (select kernels also skip empty 16-pixel blocks); sprites decode a
row of indices per tile and plot it through `obj_row_idx` / `obj_row_bmp`
(the priority rule as a masked select, 16 pixels at a time), candidate
sprites per line come from lists rebuilt only when the OAM bytes change;
`output_line` fuses copy, master brightness and expansion. Device, 400
frames from direct boot: Mario & Luigi 2D stages 2.6 s → 1.1 s (frame
15.0 → 10.4 ms), Spectrobes 2.9 s → 1.4 s (14.4 → 9.2 ms). What remains is
the tile gather (~5 %), `output_line` (memory-bound: the framebuffer is
written through to DRAM) and the OAM-order sprite loop.

**2D, second pass (2026-08-22), from the subsystem sweep.** Counters first
(`DS_PROFILE=1` prints them): per line ~2 text backgrounds and 1.6 plane
selects; BLDCNT has an effect selected on 85 % of lines, but an effect can
*reach* a pixel — a first target among the layers present (and a second one
for blending), or a semi-transparent / bitmap sprite or the 3D layer over a
second target — on only 75 % of SM64DS's lines (the 3D layer), 49 % of
Mario & Luigi's and 45 % of Meteos's. The rest now take a *flat* path
(`Engine2D::effect_possible`, conservative per line): the planes are
selected straight into the output line (`select_plane_flat` /
`select_obj_flat`), with no second-layer, kind or alpha records and no
composite pass. Device, replayed scenes: Meteos −1.8 %, Mario & Luigi
−2.2 %, SM64DS −1.0 %. Tried and dropped: caching the text-background
gather (the 33 map entries and tile pointers, reused across the eight
lines of a tile row while the map bytes match) — exact, and no measurable
change on any scene: the gather's cost is the tile-row fetches themselves.

Measured and parked: a line cache. Counting lines whose register state
matches the previous frame's same line while the engine's palette, OAM and
mapped banks are byte-identical across the frame gives SM64DS A 6 % / B
18 %, Mario & Luigi A 8 % / B 55 %, Meteos A 16 % / B 26 % of lines — worth
2–7 % of wall, but exactness needs VRAM write tracking within the frame
(a frame-end compare cannot see a mid-frame write that a skipped line
should have shown), which is a page-table write-notify mode plus DMA
hooks. Not built yet.

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
frame is rasterised line by line into a four-line ring of 258-pixel
colour/depth/attribute rows (a one-pixel border each side, and a second
layer holding the pixel underneath for anti-aliasing and translucent
blending through edges); the final pass of line y-1 runs after line y, and
the finished line is copied out to a 256x192 output buffer. Edges step with an
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

Span pipeline (2026-08-21 p.m.): a polygon's scanline is rasterised in two
stages. `span_stage` evaluates the perspective factor, depth and the five
attributes for the whole span into per-pixel arrays through the kernel twins
in `gpu/kernels.h` (`span_factor`, `span_attr_persp`, `span_attr_linear`,
`span_z_linear`; reference and NEON, four pixels per step — the NEON
divisions are correctly rounded f64 quotients, which are exact for u32/u32
because the error is below `num * 2^-53 < 1/den`). `resolve_span` then walks
the pixels with everything per-polygon hoisted into a `Shade` record, one
instantiation per (depth mode, textured, AA, shadow), so the inner loop
branches only on pixel data. Output is byte-identical to the previous
per-pixel `Interp` code on every dumped game; on the RK3566 the span stage of
SM64DS's first 300 frames went from 2.19 s to 0.96 s.

**3D pass (2026-08-21 p.m.), from the device profile and the stage
counters (`DS_PROFILE=1` now reports polygon-lines, span pixels and resolved
pixels).** Two different shapes: Metroid Prime Hunters has ~4 000
polygon-lines per frame averaging 28 pixels of which 39 % pass the depth
test, so per-line fixed cost and wasted interpolation dominate; SM64DS has
270 polygon-lines and 44 k pixels that all pass, at ~180 cycles per pixel
in the scalar resolve — in-order stalls down a long dependent chain, not
bandwidth (IPC 0.74, L1 misses negligible). Done: the clear is per line
just before the line is rendered (the final pass of line y−1 runs after line
y, so its neighbours are ready) instead of a 600 KB streaming clear; an
active polygon list per line (buckets by `ytop`, merged in list order — the
blending rules depend on list order); the `Shade` record is decoded once
per polygon; the edge and span linear interpolations divide by one multiply
with `ceil(2^32/xdiff)` and a compare fix-up (exact: the product is below
2^32 and the estimate is the quotient or one too many); depth is staged
first and `depth_candidates` marks the pixels the span can still write
(against the top pixel, or the one underneath where the top has edge
flags) so attributes are interpolated and the span resolved only in that
range; `resolve_span_vec` does four pixels per step — all four depth
modes, opaque writes with the AA push and per-lane coverage (an exclusive
prefix count across lanes for the accumulating edge parts), translucent
blending (`plot4`) for the top and the underneath pixel — with texels
gathered through `texture_gather4` (wrap/clamp/addressing on lanes, one
texel and one palette load per lane from direct pointers resolved once per
polygon); shadow, wireframe, toon and the "may land underneath" lanes go
through the scalar resolve, which remains the specification; the final
pass skips lines no polygon touched. Lane masks are tested through one
64-bit transfer of the narrowed lanes: a `umaxv` per test was a 10-cycle
stall each. The perspective factor uses a reciprocal estimate with two
Newton steps and the same integer fix-up, falling back to the f64 divide
when the quotient or the sum could wrap (`vdivq_f32` is no faster than
f64 on the A55's 64-bit FP pipe). Device: MPH 3D 7.0 → 5.1 ms/frame, SM64DS
3.6 → 3.2, Mario & Luigi 1.45 → 1.2; whole frames MPH 21.2 → 19.2 ms, SM64DS
15.6 → 15.1, M&L 10.4 → 9.9. What remains, in order: the depth/attribute
buffers as a four-line ring (only the colour buffer must persist; the
clear, the resolve's loads and the final pass then stay in L1 — ~5 % of
SM64DS is memory stalls on those 1.2 MB), the translucent blend on 16-bit
lanes, the geometry unit (6–9 %).

**Four-line ring (2026-08-22).** The working buffers are now a ring of
four rows (20 KB for both layers of all three, against 1.2 MB): line y is
rendered, the final pass of line y-1 reads rows y-2..y, and the finished
row is copied to the output buffer. The border rows are written into the
ring just before the pass that reads them. Byte-identical on the eight
baselines and the three played scenes (host, and NEON under qemu). Device,
replayed scenes: SM64DS 94.2 → 92.7 s (−1.6 %), Mario & Luigi 113.8 → 110.9
(−2.6 %), Meteos 65.3 → 63.7 (−2.5 %) — the memory stalls those buffers
were blamed for were mostly elsewhere.

**Texel gather (2026-08-21 p.m.), from the first gameplay profile.** Sixty
seconds of hand-played SM64DS put `texture_gather4` at 11 % — one generic
function switching on the format per four pixels and bouncing the lanes
through the stack. It is now one instantiation per (format, S wrap, T wrap)
chosen once per polygon (`Shade::gather4`): the wrap arithmetic and the
decode are compile-time and the lanes travel through registers. The format
mix measured on 900 direct-boot frames is 94 % A5I3 in SM64DS and 74 % /
26 % 256-/16-colour in Mario & Luigi; the compressed format (3.7 % of
SM64DS's gathers) still takes the per-lane sampler. Byte-exact against the
reference on SM64DS, M&L and MPH. What is left in the function is the
dependent texel-then-palette load pair and a stall on the coordinate
arrays the span stage has just written — memory latency, not dispatch.
On the headless 900-frame runs the whole-run gain is only 1–2 %, but those
runs are logos, menus and attract loops where the gather is 1.7 % of the
profile. With the played scenes replayed (§8) the picture is different
again: in SM64DS gameplay 77 % of the gathers are the **compressed 4x4
format**, which the attract loop never uses, and they were all going
through the per-lane sampler — 13.4 % of the scene. That format now has
its own gather (per-lane block addressing through the view tables, the
block's four decoded colours cached across lanes). It is byte-exact and it
made no difference: the same 13 % merely moved into the new symbols and
total cycles fell 1.4 %. Prefetching the next block along S did nothing
either. The cost of compressed textures is the texel, palette-info and
palette loads missing cache, not the instructions around them — and the
likeliest reason they miss is the 1.2 MB of depth/attribute buffers
streaming through the same L2 every line, which is the four-line-ring item
above.

**Texture cache (2026-08-22, `gpu/texcache.*`), from the DraStic
forensics.** DraStic never decodes a texel while rasterising: it samples
from a cache of decoded texels (private research notes, NEON coverage §4),
so its texel fetch is one load. Ours now does the same: the first polygon
to use a (format, address, size, palette, transparent-0) in a frame
decodes the whole texture into one 32-bit word per texel — the sampler's
16-bit colour in the low half, the 5-bit alpha above it — and the span
kernels gather with one independent load per lane through nine wrap-mode
instantiations. Validity is by content, not write tracking: once per frame
per texture, the source ranges (texels, the compressed format's slot-1
palette info, the palette range the decode actually touched) are compared
with the copy taken at decode time and the texture is re-decoded on any
difference, which covers bank remaps, DMA, capture and palette animation
with no hooks anywhere. DS textures are small: the SM64DS scene decodes
91 textures in 3600 frames and compares 43 KB per frame. Entries unused
for a frame are dropped past a 24 MB budget. `Renderer3D::texture_sample`
remains the specification; `tests/texcache_test.cpp` checks every format
against an independent decoder, and the NEON build (cache) against the
reference build (sampler) is byte-exact on SM64DS, M&L, MPH and 1500
frames of the played SM64DS scene. `DS_NO_TEXCACHE=1` disables it.

The cache is used where sampling from VRAM is a chain of dependent loads:
always for the compressed format, and for any texture the direct pointers
cannot cover. For the other formats a byte texel plus an L1-resident
palette beats a word from a four-times-larger decoded array — caching
everything cost Mario & Luigi and Meteos 0.5-1 %. On the replayed scenes:
SM64DS −10 % whole-run (3D span stage 66 → 54 s), the 2D-heavy games
unchanged.

**Frames without a new swap (2026-08-22).** The display runs at 60 Hz but
the 3D engine only has a new polygon list when the game issues
SWAP_BUFFERS; SM64DS swaps every other frame. We rasterised every display
frame regardless. Now `Gpu3D::vblank` notes when there was no flush and
the render registers (DISP3DCNT, alpha test, clear, fog, edge and toon
tables) match what the last render used — melonDS's `RenderFrameIdentical`
— and `Renderer3D::render` then validates every texture the polygon list
reads through the cache; if none had to be re-decoded the previous colour
buffer is kept. That is exact (unused VRAM cannot affect the picture and
everything else that could is compared), byte-identical on the eight
baselines and the three played scenes, and keeps 1 269 of the SM64DS
scene's 3 600 frames (its menus run at 60): 113.4 → 94.5 s, −17 %, on top
of the cache's −10 %; M&L and Meteos within noise (they swap every frame).

The rotscale backgrounds (affine, extended, large bitmap) sampled their
map and tiles through the out-of-line OR-read accessor twice per pixel —
the pattern the 2D pass had replaced with direct pointers for text
backgrounds, never applied here; `vram_fetch8/16` inline the unique-bank
case (Meteos: `VramMap::read8` was 3.2 % of its gameplay profile).

## 6. Sound (`spu/`)

The SPU is an ARM7-side device and runs entirely from one scheduler event:
every 2048 ARM9 cycles (32.768 kHz) `Spu::mix` advances the sixteen channel
timers by 512 SPU ticks, decodes whatever samples they cross, pans, sums,
captures and emits one stereo frame into a ring the frontend drains. Channel
data is fetched through a 32-byte per-channel FIFO in 16-byte bursts
(`bus.dma_read32` on the ARM7 map, zero below 0x4000 — the SPU cannot read
the ARM7 BIOS), so the per-sample hot path never touches the page table.

Formats: PCM8, PCM16, IMA-ADPCM (4-byte header, loop-point predictor/index
snapshot taken on the first pass through the loop start), PSG square with
the eight duty cycles (channels 8–13), 15-bit LFSR noise (14–15). Volume is
`sample << {4,3,2,0}[divider] * volume` with 127 → 128, pan the same, mixer
`>> 10`, master `>> 7`, output `>> 8`, SOUNDBIAS applied relative to 0x200,
clip to s16 — every truncation point follows melonDS so the streams can be
compared sample for sample. SOUNDCNT bits 8–11 select mixer / channel 1 /
channel 3 / both per side; bits 12–13 pull channels 1 and 3 out of the mix.
Capture units 0/1 sample the left/right mixer output (16- or 8-bit, loop or
one-shot) through a 16-byte FIFO and write it back to ARM7 memory; the
"add" and channel-source capture modes melonDS also leaves out are
reported once and ignored. POWCNT2 bit 0 mutes the output. Direct boot
leaves POWCNT2 = 1 and SOUNDBIAS = 0x200 as the firmware does.

The DS has no sound DMA: the ARM7's DMA controller has no FIFO mode, so
there is nothing to wire there. Output resampling (32768 Hz → the device
rate) and clock drift belong to the frontend, not the core.

The sample event is rescheduled from its *nominal* time, never from
`now()`: events fire at slice ends, up to one CPU overshoot late, and
rescheduling from the late time accumulated into a sample clock 0.036 %
slow — enough for Rhythm Heaven's just-in-time stream writer (it refills
each 256-byte chunk ~0.5 ms before the FIFO prefetch reaches it) to
overtake the prefetch and play next-lap samples as crackle in one channel.
The scanline, display-FIFO and hardware-timer handlers had the same
pattern (frames ran +432 ARM9 cycles, 0.038 %, long) and now reschedule
from `Scheduler::event_time()`, the firing event's deadline; a frame is
exactly 1 120 380 cycles and Rhythm Heaven matches melonDS frame for frame
and sample for sample with no offset at all.

Cost: sixteen channels × 32768 samples/s of integer work; measured under
`DS_PROFILE=1` as the `spu` stage: 0.3–0.45 ms per frame on the RK3566.

## 7. Verification

0. Every periodic event handler reschedules from `Scheduler::event_time()`
   (its deadline), never `now()` (the slice end, up to a CPU overshoot
   late): the accumulated lateness made frames 0.038 % long and the SPU
   clock 0.036 % slow before 2026-08-21.
1. Interpreter vs melonDS per-instruction trace diffs — see
   [TRACING.md](TRACING.md). Status 2026-08-20: real firmware boot (120 frames)
   and Meteos direct boot (300 frames) match with no semantic divergence on
   either CPU, 4 M distinct states each.
1b. 2D renderer vs melonDS per-pixel frame diffs (`--dump-frames`,
   `tools/compare_frames.py`). Status 2026-08-21: Meteos, Mega Man ZX,
   Bangai-O Spirits and Sonic Rush are pixel-exact over 300-400 frames apart
   from single scene-transition frames; Kirby Canvas Curse differs only in
   animation phase. AArch64 (under qemu) produces byte-identical frames.
1c. Sound vs melonDS sample for sample: `--dump-audio file` writes the mixer
   output as raw interleaved s16 stereo at 32768 Hz (≈547 samples per
   frame); the oracle build dumps the same stream and
   `tools/compare_audio.py` aligns the two (games start their streams a few
   ms apart) and counts differing samples per frame. Status 2026-08-21:
   Rhythm Heaven (streamed PCM16) and Mario & Luigi are exact over 600
   frames; Super Mario 64 DS differs from frame 27 because its sequencer
   keys notes ~21 ms apart from melonDS with a different envelope state —
   CPU timing upstream of the SPU, inaudible. `tests/spu_test.cpp` checks
   each format, the mixer arithmetic, capture and mute in isolation.
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

## 8. Frontends (`src/frontend/`)

`cli/` is the headless harness the verification chain runs on (traces, frame
and audio dumps); it has no windowing dependency and never gains one.

`sdl/` is the playable one: direct boot, both screens, sound, input, battery
saves, and nothing else — no savestates, no configuration, no menus.

* **Presentation** (`display.*`). One window holds a list of *views*
  (`{screen, rect}`), so the eventual per-screen-window mode is a second
  `Display` rather than a rewrite — textures belong to a renderer and cannot
  be shared between windows, which is what forces that shape. The handhelds
  run SDL's KMSDRM backend, where only one window can exist, so one window
  holds both screens: stacked (default) or side by side (`--layout
  horizontal`) — the latter for dual-panel units such as the RG DS, whose
  compositor lays the panels out horizontally and reports touch in that
  combined canvas, so the window's bottom screen must sit where the touch
  panel is. Scaling preserves aspect and is not forced to
  integers: a 1280x720 panel fits the 256x384 stack 1.875 times and rounding
  down to 1 would waste the screen. The core's framebuffers are ARGB8888
  already, so a frame is two `SDL_UpdateTexture` calls.
* **Sound and pacing** (`audio.*`). The device is opened at the SPU's own
  32768 Hz and SDL converts for the hardware; samples are queued rather than
  pulled from a callback, which keeps the frontend single-threaded (the SPU
  ring is the buffer). The queue depth is also the clock: a frame is a fixed
  number of samples, so holding the queue near three frames paces the
  emulator at the DS's exact rate with no timer. The wait is bounded so a
  device that stops consuming slows the emulator instead of hanging it;
  without audio, a wall-clock pacer runs at 59.8261 Hz.
* **Input** (`input.*`). Keyboard, `SDL_GameController` and touch (a real
  finger on the handhelds, the mouse elsewhere) fold into one button mask and
  pen position, applied to the core once per frame through `Io::set_buttons`
  and `Io::set_touch`. Touch maps through the bottom screen's view rect.
  KEYCNT interrupts are implemented because games wake from `halt` on them.
* **Touchscreen calibration.** Direct boot copies the firmware's user
  settings verbatim, calibration included, and a real dump carries whatever
  that console's owner calibrated (ours: ADC 632-3408 across pixels 32-224).
  `NDS::normalise_touch_calibration` rewrites both user-settings blocks to
  the identity (ADC = pixel << 4) and fixes their CRC16s, so the frontend
  reports plain pixel coordinates; melonDS does the same at reset, which
  keeps traces against it comparable.
* **Battery saves.** The core emulates the save chip but never touched a
  file; the frontend loads `<rom>.sav` at start and writes it back when the
  chip is dirty. Savestates are deliberately absent, battery saves are not
  optional.
* **Input record/replay** (`core/input/input_log.*`). The gameplay profiles
  that direct the renderer work (§5.2) come from hand-played sessions, and a
  renderer change measured on headless direct-boot runs sees logos and
  menus rather than the scene that was hot. `--record` writes one 8-byte
  record per frame (button mask, pen x/y, pen down) and `--replay` feeds it
  back before each `run_frame` — in the window, or in the headless CLI,
  which then runs for the log's length and takes `--dump-frames` and
  `perf` like any other run. The emulator is deterministic given its
  inputs (two replays produce byte-identical frames), so the replay
  reproduces the session provided the ROM, BIOS and battery save match;
  the log records nothing else on purpose.

**Measured on the RK3566** across SDL's KMSDRM backend (no session) and sway,
and across both graphics stacks — Mesa 26.1.6 / panfrost (SDL's `opengl`
renderer) and the Mali blob (GLES-only, so SDL's `opengles2`) — presentation
costs a flat **0.7-1.5 ms per frame** whatever the emulation load, with and
without vsync — under a tenth of the 16.7 ms budget. The whole spread across
those four combinations is 0.8 ms (best: the blob under a compositor, where
the flip happens on the compositor's core instead of the emulator's thread),
so the graphics stack is not where the time goes. With vsync on, the
present call also absorbs the slack when the emulator is ahead (up to 5 ms),
which is waiting rather than work; audio pacing holds the queue at 2-3
frames. `--frames N` with `DS_FPS=1` prints a line per 60 frames;
`--no-vsync --no-audio` removes every sleep and leaves the real cost.
Comparing two runs line by line only holds if neither is touched — the
emulator is deterministic, the player is not.

Everything else is emulation, and the frontend adds nothing measurable to
it: over the same frame windows the headless CLI costs the same to a tenth
of a millisecond (SM64DS 21.0 ms both; Mario & Luigi 9.7 vs 9.6 and 26.6 vs
26.7). Comparing a CLI *average* over a window against the frontend's last
blocks inside it does not work — cost ramps steeply when a game reaches a
heavy scene, and that shape is the real finding: 900 untouched frames from
direct boot reach 15.6 ms in Meteos (98 %), 21.0 ms in SM64DS once its 3D
attract starts (75 %) and 33 ms in Mario & Luigi from frame ~720 (45 %).
The 300-frame averages of §5 describe logos and menus. Closing that gap is
§7.4's problem, not the frontend's.

Battery saves change what a game boots into and therefore what it costs, so
a measurement pair must have identical (or no) save files.

## 9. Targets

| tier | device | role |
|---|---|---|
| low | Anbernic H700 | regression |
| mid | RK3566 | **the benchmark device** (only one carrying both DraStic and melonDS) |
| upper-mid | T618 | regression |
| high | Retroid Pocket Mini V2 (SD865) | regression |

Development on x86 runs the interpreter and C++ kernels; `cmake --preset
aarch64-cross` + `qemu-aarch64-static` exercises the JIT/NEON paths before a
device does.
