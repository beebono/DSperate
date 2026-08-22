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
instructions), laid out as a *hot* section (the fast paths, in guest order)
followed by a *cold* section (everything reached only on a rare condition:
budget exhausted, page-table miss, block-transfer fallback). The cold code is
written to a side buffer during translation and spliced in after the hot
code; cross-section branches and cold `bl`s to the stubs are fixups resolved
at the splice. The hot section runs 9–10 host words per guest instruction
(SM64DS: 36.8 bytes; the cold third is never fetched).

**Stubs take literal arguments.** A block reaches a stub with `bl`; the stub
reads its operands from the words after the `bl` through x30 and skips them:
`bl fallback; .word instr; .word key` (3 words, was ~30 inline),
`bl poll; .word next_key`, `bl link; .word key`. A linked `bl link` becomes a
bare `b` whose literal is never executed, so a linked branch costs one
instruction. Indirect branches go through a per-CPU stub that updates T,
charges the pipeline refill and looks the key up in a 64 K-entry
direct-mapped LUT (`(native offset << 32) | key`, key = pc | T), falling back
to a hash map and then to translation. The budget register holds
`budget - 1`, so every budget test is one `tbnz w8, #31` (the stubs add and
subtract the one at the C boundary).

**Memory.** Loads and stores inline the tagged page-table fast path
(`lsr, ldr, lsl, cbz, ldr`); stores also test the two tag bits, so MMIO,
read-only and *code* pages leave the fast path. The timing-table load and the
address arithmetic are issued under the page-entry load, and the charge under
the data load, so the in-order A55 stalls on neither. The slow path (cold)
calls `jit_h_ld*/st*` — the interpreter's own `mem_read*/mem_write*` paths
minus the cost — through a pure-call stub that spills only the caller-saved
guest registers, then repeats the tail (writeback, cost) and polls; the
whole instruction is never re-executed by the interpreter. LDM/STM/PUSH/POP
take the fast path when the whole transfer sits in one 2 KB page and fall
back to the interpreter otherwise.

**Fallback.** Anything not inlined — MSR that changes mode, SWI, coprocessor
reads, PC-destination ALU ops, user-bank transfers, SWP, LDRD/STRD, the v5
DSP extensions, undefined encodings — calls `jit_h_fallback`, which executes
that one instruction through the interpreter with the interpreter's own
cycle accounting, then polls budget / alerts / IRQ and returns to the block
or dispatches on the new pc. The JIT is therefore complete and exact from
the first build; inlining is an optimisation, and every inlined form is
checked against the interpreter by `tests/jit_test.cpp`. Inlined since the
first build: CP15 cache-maintenance `MCR`s the core ignores (`numC + 2`),
and `MSR CPSR_<fields>` when the mode does not change (tested at run time;
I/F go to the memory copy, the flags field to the host NZCV, and the block
polls for an unmasked IRQ).

**Cycles.** The interpreter's model (`cpu_cycles.h`) is reproduced exactly:
code-fetch cost is static per instruction (the TCM windows and PU cache map
are baked into the per-page table, so it is one lookup for both engines),
data cost is read from the same table at run time, and the CD/CDI combine
formulas — including the ARM7 main-RAM rules — are emitted inline without
touching flags. On the ARM9 `max(nc + nd - 6, nc, nd)` with `nd >= 1`
collapses to `nd` for `nc <= 1` (one `sub`), `max(nc, nd)` for `nc <= 6` and
`nc + max(nd - 6, 0)` above. Static costs are batched into one `sub` per run;
a conditional instruction whose cost is only its fetch cost charges it before
the condition test, so the skipped path needs no code. In `DS_JIT_STRICT=1`
mode the budget is also tested after every instruction, so the two engines
interleave identically and frame dumps must be byte-equal; normally it is
tested once per block.

**Flags.** A backward liveness pass per block records which of N, Z, C, V are
live after each instruction. Logical ops then use a bare `tst` when C/V are
dead; when they are not, a shared stub merges (`bl merge_keep_cv` /
`merge_set_c`, two or three words at the site). Arithmetic maps to
`adds/subs/adcs/sbcs` directly (same carry convention).

**Self-modifying code.** Code pages are tracked by *host* page so that the
other CPU's mapping and DMA see the same `TAG_CODE` bit; remaps re-apply it
through `PageTable::code_query`, and `set_code_host` finds the guest pages of
a host page through a reverse index rebuilt lazily after a remap. Every store
path that lands on a tagged page reports through `mem::code_written`; the
runtime kills the blocks on that page (entry patched to a redirect into the
dispatcher, LUT/map entries removed) and raises the alert word so a block
currently executing leaves at its next poll.

**Interrupts** are taken on the C side: translated code leaves when a poll
finds `irq_pending` set with I clear, `run()` calls `check_irq()`, and
re-enters at the new pc. Between slices the scheduler's events set
`irq_pending`, and `run()` checks it first, as the interpreter does. A halt
inside a helper raises `ALERT_HALTED`; `run()` records `budget_at_halt` and
ends the slice, exactly as the interpreter's loop does.

## Verification

1. `tests/jit_test.cpp` (cross-built, runs under qemu): random straight-line
   ARM and Thumb sequences on both CPUs, run by the interpreter and the JIT
   from identical state; registers, flags, consumed cycles and memory must
   match. Failures are shrunk to the shortest failing prefix. Directed cases
   cover what the generator reaches rarely (pending cycles across a slow
   access, CP15 no-ops, MSR with and without a mode change). Every trial ends
   in a halt, and both engines end a slice with budget −1, so the consumed
   cycles are compared through `CpuContext::budget_at_halt` — until
   2026-08-21 the budget comparison was −1 against −1 and saw nothing; the
   lost-pending-cycles bug below dates from then. Bugs it has caught so far:
   CLZ inlined on the ARM7, unaligned Thumb LDM bases, the ARM7 multiply
   clearing C, register shifts by 64/96/128 taking the "exactly 32" carry
   rule, a fallback instruction observing flags the liveness pass had marked
   dead, and the static cycles pending before a load being charged only on
   its fast path.
2. `dsperate --interp` vs default on the same ROM with `--dump-frames`
   (`DS_JIT_STRICT=1` for byte equality) and `--trace` (the JIT calls the same
   trace hook per instruction when tracing is on).
3. Against melonDS exactly as the interpreter is (docs/TRACING.md).

## Debugging

- `DS_JIT_STRICT=1`: per-instruction budget checks (lockstep with the interpreter).
- `DS_JIT_DEBUG=1`: log every fallback with pc, instruction and resulting state; dump every translated block as hex words.
- `DS_JIT_HIST=1` (with `DS_PROFILE=1`): fallback counters and the hottest fallback sites at exit.
- `DS_JIT_FASTCOST=1`, `DS_QUANTUM=<cycles>`: measurement knobs, inexact (`DS_JIT_FASTCOST=1` is also the mutation check for the fuzzer: `test_jit` must fail with it).
- `DS_DEBUG_TIMING=1`: log every ARM9 timing-table rebuild (each one drops every translated block).
- `--interp`, `--jit9`, `--jit7`: choose the engine per CPU.

## Measured on the RK3566 (Cortex-A55 @ 1.99 GHz)

Super Mario 64 DS, 300 frames from direct boot, `DS_PROFILE=1` (CPU stage
times; `perf` works on the device: `perf stat`/`perf record` with the A55
PMU, JIT code shows as `[JIT]`):

| build | ARM9 | ARM7 | frame (profiled sum) |
|---|---|---|---|
| interpreter | 9.44 s | 1.16 s | 51 ms |
| JIT, first build (2026-08-21 a.m.) | 2.91 s | 1.11 s | 28.9 ms |
| + cold sections, literal-argument stubs, slow-path helpers, ARM9 charge collapse | 2.46 s | 0.99 s | 27.1 ms |
| + MSR/MCR inline, no-op CP15 writes skipped, `set_code_host` index, LUT-first entry | 1.75 s | 0.71 s | 23.8 ms |
| + cached next deadline, DMA running mask | 1.72 s | 0.70 s | 23.7 ms (25.4 wall) |
| + 2D/3D renderer passes (not the JIT), range-limited timing rebuilds | 1.52 s | 0.69 s | 15.1 ms |
| + native slice loop (2026-08-22) | 1.55 s | 0.65 s | 14.9 ms wall (MPH 19.1, M&L 10.4, Spectrobes 8.6) |

The game executes ~292 k ARM9 instructions per frame; translated code is
**13.7 % of the run** (≈3.3 ms/frame for both CPUs, ≈18 host cycles per
guest instruction). The I-cache theory of the first build was wrong: `perf
stat` shows 18.8 M L1I misses in 300 frames (~3 % of cycles). What the
profile actually showed, and what was done:

- `Timing::update_cpu9` (7 %) and `PageTable::set_code_host` (6 %): every
  CP15 TCM/PU write rebuilt the 1 M-entry timing table and dropped every
  block, even when the value was unchanged; every code-tag flip scanned
  128 k page entries. Now: unchanged writes are skipped, `update_tcm` only
  remaps when the windows changed, and the tag flip uses a reverse index.
  What remains of `update_cpu9` is boot.
- Fallbacks: 2.14 M → 0.19 M per 300 frames (memory slow paths are pure
  calls; CP15 cache ops and same-mode MSR are inline).
- Per-slice overhead (scheduler loop, `enter`/`exit`, `run()`): ~14 % of the
  run, ~450 ns per 128-cycle slice, 7.3 k slices per frame.

**Native slice loop** (done 2026-08-22, `Runtime::run_loop`,
`Scheduler::slice_next`): the scheduler's slice sequence runs from a loop
in the arena that saves the callee-saved registers once and enters blocks
through `enter_light` (x29/x30 only); `enter` for C callers wraps it. The
C++ slice logic became a straight-line function with two resume points —
the `switch` state machine tried first was slower than the old loop
(jump-table and indirect-call mispredicts on the A55). Worth 2–6 % of wall
time; `slice_next` is still ~6 % (≈300 cycles per slice) and the guest
register save/restore per entry is the rest. Next on this axis: refilling
the budget in place from the poll stub while the other CPU is halted and
no event is due, so the ARM9 never leaves translated code between slices.

Next, in order, each measured on the device with the strict slice diff kept
green:

1. The translated code itself (≈18 cycles per guest instruction): the
   dispatcher for indirect branches (`bx lr`: LUT probe + refill-cost stub,
   ~45 instructions and an indirect-branch mispredict), conditional
   execution without the pending flush, link through a second entry point
   that skips the budget test, shift-by-register clamps.
2. ARM7 data-cost path (still the full main-RAM rule inline, ~12
   instructions); inline `MSR SPSR` and `LDR pc` for the ARM7 BIOS IRQ
   handler (MPH's ARM7 spins in BIOS `swi 3` with five fallbacks per loop).

Still open, not performance: W^X dual mapping for Android; inlining the v5
DSP ops, SWP, LDRD/STRD; ARM7 straight-line code crossing a 32 KB region
boundary uses the block's own region for fetch costs (the interpreter keeps
the last jump target's).


## Profiling translated code

`DS_PERF_MAP=1` writes `/tmp/perf-<pid>.map` as blocks are translated, so
`perf` resolves JIT samples to `jit9_<pc>` / `jit7_<pc>` (a `t` suffix marks
Thumb) instead of one anonymous mapping. The arena is reused after a flush,
so an address can appear more than once; perf takes the last entry.

Measured this way on the RK3566 over 60 s of real gameplay, translated code
is 6-11 % of the process and extremely diffuse — the hottest single block is
0.65 % (Mario & Luigi) and 0.19 % elsewhere. The renderer, not the
recompiler, is where the time goes.
