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
costs 1. Each instruction is charged a code fetch plus, by class, internal
cycles (CI), a data access (CD) or a load (CDI), overlapped the way melonDS
overlaps them; branches pay the pipeline refill. Measured against melonDS on
Meteos: instructions per frame agree to ~0.1% on the ARM9 and ~3% on the ARM7.

**DMA** (`dma/`): eight channels with immediate/VBlank/HBlank/display-start/cart
triggers, main-RAM burst unit timings, repeat and IRQ. GX FIFO and display-FIFO
modes wait for the 3D and 2D engines.

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
  its rasteriser architecture is **not yet documented** on the research side.

## 6. Verification

1. Interpreter vs melonDS per-instruction trace diffs — see
   [TRACING.md](TRACING.md). Status 2026-08-20: real firmware boot (120 frames)
   and Meteos direct boot (300 frames) match with no semantic divergence on
   either CPU, 4 M distinct states each.
2. JIT vs interpreter differential execution, block by block — the first
   divergence names the broken instruction.
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
