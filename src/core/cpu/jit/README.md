# ARM → AArch64 recompiler

Builds on AArch64 hosts only (`DSPERATE_JIT`); the x86 build runs the
interpreter. The design is summarised in `docs/ARCHITECTURE.md` §3; the full
derivation is in private research notes that are not published.

**Every line here is authored from that design description.** Nothing is
transcribed from `objdump`/Ghidra output of any proprietary binary. melonDS's
`ARMJIT_A64` (GPLv3) may be consulted and, with attribution, reused; so far
nothing has been.

## Files

| file | role |
|---|---|
| `emit.h` | AArch64 encoder: only the forms the translator and stubs use. Verified against `aarch64-linux-gnu-objdump`. |
| `runtime.cpp` | code arena, stubs (entry/exit, call trampolines, dispatcher, block linker, indirect branch), block cache, LUT, SMC tracking by host page, the helpers translated code calls |
| `translate.cpp` | one-pass block translator for ARM and Thumb, flag-liveness pre-pass, cycle accounting |
| `jit.h` | public API: `attach`, `run` (a `RunFn`), `flush`, `set_trace`, `stats` |
| `jit_internal.h` | register convention, context offsets, block/runtime structures |

## How it works

**Pinned registers.** Guest r0–r7, r13, r14 live in x19–x28 (callee-saved),
r8–r12 in x9–x13 (spilled by the call stubs), the cycle budget in w8, the
page-table base in x14, the per-page timing table in x15, `CpuContext*` in
x29. Guest NZCV live in the host NZCV; the rest of CPSR stays in memory.
Because every block agrees on this, linked blocks reconcile nothing.

**Blocks** are straight-line runs ending at the first branch (or 64
instructions). Each has a five-instruction prologue (`budget <= 0 → exit`).
Static branch targets are linked lazily: the first execution goes through
`jit_h_link`, which patches the `bl` into a bare `b` to the target block.
Indirect branches go through a per-CPU stub that updates T, charges the
pipeline refill and looks the key up in a 64 K-entry direct-mapped LUT
(`(native offset << 32) | key`, key = pc | T), falling back to a hash map and
then to translation.

**Memory.** Loads and stores inline the tagged page-table fast path
(`lsr, ldr, lsl, cbz, ldr`); stores also test the two tag bits, so MMIO,
read-only and *code* pages leave the fast path. LDM/STM/PUSH/POP take the
fast path when the whole transfer sits in one 2 KB page.

**Fallback.** Anything not inlined — MMIO accesses, MSR, SWI, coprocessor,
PC-destination ALU ops, user-bank transfers, SWP, LDRD/STRD, the v5 DSP
extensions, undefined encodings — calls `jit_h_fallback`, which executes that
one instruction through the interpreter with the interpreter's own cycle
accounting, then polls budget / alerts / IRQ. The JIT is therefore complete
and exact from the first build; inlining is an optimisation, and every
inlined form is checked against the interpreter by `tests/jit_test.cpp`.

**Cycles.** The interpreter's model (`cpu_cycles.h`) is reproduced exactly:
code-fetch cost is static per instruction (the TCM windows and PU cache map
are baked into the per-page table, so it is one lookup for both engines),
data cost is read from the same table at run time, and the CD/CDI combine
formulas — including the ARM7 main-RAM rules — are emitted inline without
touching flags. Static costs are batched into one `sub` per run. In
`DS_JIT_STRICT=1` mode the budget is also tested after every instruction, so
the two engines interleave identically and frame dumps must be byte-equal;
normally it is tested once per block.

**Flags.** A backward liveness pass per block records which of N, Z, C, V are
live after each instruction. Logical ops then use a bare `tst` when C/V are
dead and the `mrs`/`bfi`/`msr` merge only when they are not. Arithmetic maps
to `adds/subs/adcs/sbcs` directly (same carry convention).

**Self-modifying code.** Code pages are tracked by *host* page so that the
other CPU's mapping and DMA see the same `TAG_CODE` bit; remaps re-apply it
through `PageTable::code_query`. Every store path that lands on a tagged page
reports through `mem::code_written`; the runtime kills the blocks on that
page (entry patched to a redirect into the dispatcher, LUT/map entries
removed) and raises the alert word so a block currently executing leaves at
its next poll.

**Interrupts** are taken on the C side: translated code leaves when a poll
finds `irq_pending` set with I clear, `run()` calls `check_irq()`, and
re-enters at the new pc. Between slices the scheduler's events set
`irq_pending`, and `run()` checks it first, as the interpreter does.

## Verification

1. `tests/jit_test.cpp` (cross-built, runs under qemu): random straight-line
   ARM and Thumb sequences on both CPUs, run by the interpreter and the JIT
   from identical state; registers, flags, consumed cycles and memory must
   match. Failures are shrunk to the shortest failing prefix. Bugs it has
   caught so far: CLZ inlined on the ARM7, unaligned Thumb LDM bases, the
   ARM7 multiply clearing C, register shifts by 64/96/128 taking the
   "exactly 32" carry rule, and a fallback instruction observing flags the
   liveness pass had marked dead.
2. `dsperate --interp` vs default on the same ROM with `--dump-frames`
   (`DS_JIT_STRICT=1` for byte equality) and `--trace` (the JIT calls the same
   trace hook per instruction when tracing is on).
3. Against melonDS exactly as the interpreter is (docs/TRACING.md).

## Debugging

- `DS_JIT_STRICT=1`: per-instruction budget checks (lockstep with the interpreter).
- `DS_JIT_DEBUG=1`: log every fallback with pc, instruction and resulting state; dump every translated block as hex words.
- `DS_JIT_HIST=1` (with `DS_PROFILE=1`): fallback counters and the hottest fallback sites at exit.
- `DS_JIT_FASTCOST=1`, `DS_QUANTUM=<cycles>`: measurement knobs, inexact.
- `--interp`, `--jit9`, `--jit7`: choose the engine per CPU.

## Measured on the RK3566 (Cortex-A55 @ 1.99 GHz, 2026-08-21)

Super Mario 64 DS, 300 frames, `DS_PROFILE=1`: ARM9 interpreter 9.41 s,
JIT 2.89 s (strict mode 3.1 s; strict frames byte-identical to the
interpreter on device). The game executes ~292 k ARM9 instructions per frame,
so the JIT costs **66 host cycles per guest instruction** (interpreter: 213).
Fallback executions: ~7,100 per frame (GX-FIFO poll `LDR`s and the IRQ
prologue's CP15/MSR dominate). Native entries: one per 128-cycle slice,
~7,300 per frame. Measurement knobs (inexact, never for real runs):
`DS_JIT_FASTCOST=1` (constant data cost) saves 5 %, `DS_QUANTUM=1024` saves
11 %, both 18 %. The whole frame is 28.8 ms against a target of ~4.9 ms (the
DraStic baseline in docs/ARCHITECTURE.md §6 scaled to this clock).

## Next stage: code size

The emitted code is ~30 host instructions per guest instruction (every
fallback call + poll, every cost formula, every flag merge is inline at
every site), about 5 MB for 56 k translated instructions. That does not fit
an A55's 32 KB L1I; the core is stalled on instruction fetch, which is why
the cost knob barely moves the needle. Targets, in order, each measured on
the device with the strict slice diff kept green:

1. **Fast path ≤ 8 host instructions per guest instruction.**
   - Fallback site = `mov w1,#instr; mov w2,#key; bl fallback_stub` (3
     instructions); the stub does the full sync, call, poll and the
     jumped/not-jumped dispatch. Same for trace calls.
   - Slow memory paths out of line: one shared stub per access form; the
     inline sequence is the page lookup, the access and a `b.cc`/`cbz` to
     the stub.
   - Data cost: a per-page byte table of the *combined* CD/CDI charge for the
     handful of numC values a page sees, so a load charges with one `ldrb` +
     one `sub`; the ARM7 main-RAM rule folded the same way.
   - Flag merge (`mrs/bfi/msr`, 5–6 instructions) as a shared stub when C/V
     are live; liveness already removes it from most Thumb code.
   - Exits: record (native pc → guest pc) per block instead of `movz/movk`
     at every exit (the design's PC-metadata table).
2. **MMIO load/store helpers as pure calls** (spill 5 registers, call
   `Bus::read/write`, reload), replacing the full-sync interpreter fallback
   for the common MMIO case. Hits the GX FIFO stores and the GXSTAT poll in
   every 3D game.
3. **Native slice switch**: the ARM9→ARM7→events hand-off without returning
   to C every 128 cycles; then measure what a larger quantum costs in melonDS
   agreement before changing it.
4. After those, the micro-optimisations: shift-by-register clamps, link
   through a second entry point that skips the budget test, conditional
   execution without the pending flush.

Still open, not performance: W^X dual mapping for Android; inlining the v5
DSP ops, SWP, LDRD/STRD, MSR; ARM7 straight-line code crossing a 32 KB
region boundary uses the block's own region for fetch costs (the interpreter
keeps the last jump target's).
