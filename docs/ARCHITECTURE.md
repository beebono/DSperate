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

## 4. Scheduler: a downward budget is the only check

*Source: research note "JIT design forensics" §4–5 (private); still to be
refined by a scheduler forensics pass.*

Time is ARM9 cycles in a `u64`. The scheduler hands each CPU a budget equal to
the distance to the next event; the engine runs until the budget's sign bit
sets. The ARM7 is issued half the cycles the ARM9 actually ran. Events are a
fixed-size table indexed by `EventId` (no allocation in the hot path).

This is deliberately simple; the interleave granularity and the IPC/FIFO
latency it implies need validating against real titles and melonDS.

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

1. Interpreter vs melonDS trace diffs (register file, CPSR, touched memory)
   on real ROMs.
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
