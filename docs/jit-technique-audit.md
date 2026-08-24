# The recompiler, checked against the techniques

This is an audit of **our** JIT (`src/core/cpu/jit/`) against the techniques
described in [techniques/01-arm-to-aarch64-jit.md](techniques/01-arm-to-aarch64-jit.md).
It is not a description of DraStic; it is a list of where we match, where we
have nothing, and — the interesting category — where we implement the same
idea in a shape that costs more than it saves.

## The number that frames everything

From [techniques/00](techniques/00-method-and-measurements.md): DraStic runs
SM64DS at 2.98 ms/frame, of which translated code is 14.7 % and the JIT
runtime another 12.0 %. At 2 GHz that is ~1.59 M host cycles per frame for CPU
emulation. SM64DS executes ~292 k ARM9 instructions per frame; with the ARM7 on
top, call it ~350 k guest instructions.

> **~4.5 host cycles per guest instruction.**

Our own README measures us at **~18 host cycles per guest instruction**
(3.3 ms/frame of translated code for both CPUs). Our hot sections emit
**36.8 bytes — about nine host instructions — per guest instruction**.

A one-instruction guest ALU op should be one host instruction. Nine is the
symptom. Everything below is an attempt to say where the other eight went.

---

## Matched, and matched well

These are done, and done the way the technique describes. They are not the
problem and should not be disturbed.

| Technique | Our implementation |
|---|---|
| All 15 guest GPRs pinned in host registers | `host_reg()` in `jit_internal.h`: r0–r7,r13,r14 → x19–x28, r8–r12 → x9–x13 |
| Callee-saved half chosen for free spills across C calls | We put **ten** guest registers in the AAPCS64 callee-saved range against DraStic's nine, and the Thumb-visible ones (r0–r7) at that. This is arguably better than the technique |
| Guest CPSR flags live in host NZCV | Throughout; `ADDS` → `adds`, no software flag word |
| Downward cycle counter tested by sign | `w8` holds `budget - 1`; every check is one `tbnz w8, #31` |
| Static cycle costs accumulated, one `sub` per run | `add_pending` / `flush_pending`, `translate.cpp:342` |
| Two-instruction block prologue | `emit_budget_check`, `translate.cpp:352` — one hot `tbnz`, the exit is cold |
| Direct block linking | `bl link; .word key` patched to a bare `b` (`jit_h_link`, `runtime.cpp:723`). A linked branch is one instruction |
| Flat 16 MB page table, 2 KB pages, pre-biased base, `entry << 2`, two tag bits, zero = slow path | `mem/page_table.h` — a faithful reimplementation, including "loads never test the tags" |
| Direct opcode emission, no IR | `emit.h` |
| Translate-once-and-forget | No tiering, no profiling |

Two things we have that the technique does not describe at all, and that look
like real wins:

- **Hot/cold block splitting.** Rare paths (budget exhausted, page miss,
  transfer fallback) go to a side buffer spliced in after the hot code, so the
  I-cache only holds what runs. `translate.cpp:242-280`.
- **Backward flag liveness.** Logical ops use a bare `tst` when C and V are
  dead. `arm_flag_use` / `thumb_flag_use`, `translate.cpp:128-200`.

---

## Missing outright

### 1. The ITCM dispatch tables (technique §3c)

We have nothing. The ARM9's 32 KB ITCM is where DS games put their hottest
code, and the technique's answer is a tag-free direct-indexed table — 8192 ARM
slots, 16384 Thumb slots, **six instructions, no tag compare, no possible false
hit**. Every ITCM branch in our build goes through the generic 64 K LUT probe
with a tag compare and a miss path.

### 2. Silent-store elimination (technique §5, stage 2) — measured, not worth it

**Do not do this.** Stores that leave the fast path because they land on a
code page are **0.1 % of slow accesses in SM64DS (4,434 of 4.0 M), 1.4 % in
Mario & Luigi, 0.09 % in Meteos** — measured 2026-08-24 with a temporary
per-cause split in the slow-path helpers. The filter the technique calls "the
filter that makes the whole scheme viable" would be filtering something that
barely happens here. The rest of this section stands as a description of the
gap; it just costs nothing to leave open.

The technique calls this "the filter that makes the whole scheme viable", and
we do not have it. Our store to a tagged code page goes: cold branch →
`call_pure` (spill five guest registers, budget, flags) → `jit_h_st32` →
`write_ptr` → `mem::code_written` → `invalidate_host_page`, which **kills every
block on the 2 KB host page unconditionally** and raises `ALERT_INVALIDATED`,
forcing every running block on both CPUs to leave at its next poll.

A game re-uploading an identical overlay, or memsetting a region that already
holds those bytes, pays all of that. The technique pays one load and one
compare and returns.

Two sub-gaps here:
- No value compare before invalidating (`runtime.cpp:667`).
- Invalidation is **2 KB-page granular**. The technique's stage 3
  (`cpu_block_check_allocation32`) asks whether a translated block actually
  covers *this address*. Data written next to code in the same 2 KB page
  retranslates the code.

### 3. Constant tracking (technique §7)

We have the *encoding* half — `and_imm`/`orr_imm`/`eor_imm` try logical
immediates, `mov_imm` picks MOVZ/MOVN/MOVK — but no per-register known-value
slot, so nothing propagates between instructions in a block. ARM code
manufactures constants constantly through literal pools and `MOV`/`ORR` pairs,
and every one of them reads a host register here.

### 4. The I/O fast path (technique §8) — done, but not as the technique writes it

**Fixed 2026-08-24: −3.5 % / −2.8 % / −1.6 % on sm64 / mlbis / meteos.**

Measured first, over 600 frames of each replayed scene: **99 % of everything
that leaves the inline page-table path is MMIO**, and four in five are
**loads, not stores** (sm64 6,729 slow accesses/frame, mlbis 5,297, meteos
3,881). They concentrate hard on polled status bits — Mario & Luigi reads
ROMCTRL 3,559 times a frame (67 % of its slow accesses), SM64DS reads DIVCNT
2,432 times a frame (36 %) — and the geometry ports the technique singles out
are only 10.7 % of SM64DS's traffic and ~2 % elsewhere.

Skipping the polls is not available: `cpu/idle_loop.cpp` already implements
idle-loop detection and **deliberately excludes** ROMCTRL and the cartridge
ports, because skipping them changed output in Bowser's Inside Story and
Meteos; `DS_IDLE_SKIP=1` also measures ~2 % *slower* overall. The poll had to
be made cheap.

The cost turned out to have nothing to do with the guest, the recompiler, or
the technique. Disassembling `Io::read` showed **61 compare-and-branch
instructions and no jump table** — a switch over ~25 scattered 32-bit
addresses does not become one — plus a `-fstack-protector-strong` guard and a
full frame on every call, courtesy of a `bool&` out-parameter whose
address-taken local triggered it. Returning that flag by value dropped the
guard and let GCC emit a real table (61 branches → 19); one range test
replaced three `owns_reg` probes; and the JIT's slow helpers now call `Io`
directly rather than through `Bus`'s two re-testing frames.

A hand-written short-circuit for the eight hottest addresses, built first as
an upper bound on what any dispatch flattening could recover, was **beaten**
by the general fix — it also helps the registers the histogram does not name.

Two lessons worth keeping. The technique's specific advice (recognise the
geometry ports inline in the store helper) was the *less* valuable half of
its own idea for us. And the win was found by disassembling the function, not
by reasoning about instruction counts.

3D games push their entire display list through `0x4000400`–`0x40005FC`, one
32-bit store per command word — 82,662 per frame in SM64DS, 1.27 % of all guest
instructions executed. The technique recognises those addresses **inline in the
store helper** and branches straight to the command queue.

Ours takes the full generic route: cold path → `call_pure` → `jit_h_st32` →
`Bus::write32` → `Bus::io_write` → `Io::write` → `Gpu3D::owns_reg` →
`Gpu3D::write` → range check → `gxfifo_write`, then reloads five guest
registers, the budget, both table pointers and the flags. Roughly 50 host
instructions and six nested C calls where the technique spends about eight
instructions.

(Unrelated but adjacent: `Io::write` calls `std::getenv("DS_DEBUG_GPUREG")` on
every ARM9 2D register write — `io/io.cpp:685`. That is a `getenv` per store.)

### 5. Separate arenas (technique §6)

One 64 MB arena; `reset_arena` throws away **everything**, both CPUs. Dead
blocks are never reclaimed, so ITCM churn — the thing the technique isolates
into its own arena specifically because it is invalidated far more often than
anything else — walks the arena toward a full reset that also discards every
main-RAM translation. `invalidate_cpu`, called on every timing-table change,
likewise drops a whole CPU's blocks.

### 6. The second entry point that skips the budget test (technique §3a)

Already on our own list. A link chooses the callee's `[0]`; the technique
chooses between `[0]` and `[8]`. One not-taken branch per block — the smallest
item here, and worth doing last.

---

## Implemented, but in a shape that costs more than it saves

This is the category that matters. Each of these is *the same idea as the
technique*, built in a way that adds a fixed per-operation cost the technique
does not pay.

### A. Runtime cycle accounting on every memory access

**The single biggest divergence.** The technique accumulates cycle cost *in the
translator* and emits one `sub w12, w12, #total` per straight run — "a straight
run of thirty guest instructions costs one subtract, not thirty". We do that
for *fetch* costs (`add_pending`), and then throw the saving away on data
accesses: every load and store emits a runtime timing-table lookup plus an
inline combine formula.

`emit_data_cost` (`translate.cpp:616`) is three instructions — `lsr`, `add`,
`ldrb` — off a dedicated pinned register (`x15`) that exists only for this.
`emit_charge_data` (`translate.cpp:630`) is then:

- ARM9: 1 instruction when `numC <= 1`, otherwise **4** (`sub`/`bic`/`add`/`sub`).
- ARM7: a **branch** plus up to **~14** instructions for the main-RAM overlap rule.

Counting an ARM9 `LDR r0, [r1, #4]` end to end on our hot path:

```
add   w1, w20, #4          ; EA
lsr   x2, w1, #11          ; \
ldr   x2, [x14, x2, lsl#3] ;  | technique: 5 instructions, total
lsr   w6, w1, #12          ; \
add   x6, x15, x6, lsl#2   ;  | timing lookup  (3)
ldrb  w6, [x6, #2]         ; /
and   w4, w1, #~3          ;
lsl   w5, w1, #3           ;
lsl   x3, x2, #2           ;  |
cbz   x3, cold             ; /
ldr   w0, [x3, w4]         ;
sub   w4, w6, #nc          ; \
bic   w4, w4, w4, asr#31   ;  | CD combine    (4)
add   w4, w4, #nc          ;  |
sub   w8, w8, w4           ; /
rorv  w19, w0, w5          ;
```

**16 instructions where the technique spends 6.** Seven of the extra ten are
cycle accounting. On the ARM7 store path it is worse, and it contains a branch.

The technique reaches its accuracy differently: cost is per-CPU and static
(`the ARM7 path doubles the instruction's code cycles, the ARM9 path adds a
system-wide adjustment word`), with per-game hacks providing the correction.
We chose exact melonDS timing instead — a legitimate choice, and it is what
`DS_JIT_STRICT` and the frame-diff harness rest on — but the price is being
paid on the hottest path in the emulator, per access, forever.

Worth measuring before deciding: `DS_JIT_FASTCOST=1` already exists as an
inexact knob. Run it on the device and see what exactness is actually costing
us. If the answer is large, the shape to aim for is a *statically resolvable*
data cost wherever the page can be determined at translate time (stack, TCM,
main RAM through a known base) with the runtime path kept only for genuinely
unknown addresses.

### B. The indirect-branch dispatcher

Same idea as technique §3b — a direct-mapped cache keyed on guest PC — built at
roughly five times the cost.

The technique: **nine instructions, two loads, no call, no hash**, on a 1024-entry
table (4 KB tags + 4 KB values) that sits *inside the CPU state struct* at a
fixed offset from the already-pinned context register, holding **32-bit
cache-relative offsets**.

Ours (`runtime.cpp:437` → `runtime.cpp:298`) for a plain `bx lr` on the ARM9:

| stage | cost |
|---|---|
| `mov w0, w28`; `b branch_indirect` | 2 |
| `mrs x17, nzcv` … `msr nzcv, x17` around the body | 2 (each ~6 cycles on an A55) |
| T-bit update through the CPSR memory copy | 4 |
| pipeline-refill charge: two `emit_fetch_cost9` expansions | **~15, with branches** |
| `dispatch`: `mov_imm64 x1, <lut>` | **4** |
| probe: `ubfx`/`ldr`/`eor`/`cbnz`/`lsr` | 5 |
| `mov_imm64 x5, <arena>` | **4** |
| `add`/`br` | 2 |

**~47 instructions**, versus nine. Four separate problems, each independently
fixable:

1. ~~**The LUT address is materialised with a 4-instruction `mov_imm64` on every
   dispatch**, and the arena base with another.~~ **Done, 2026-08-24 — but it
   bought nothing measurable.** Both LUTs moved to the front of the code arena
   and the arena base pinned in `x18`, so one register serves the probe and the
   jump. The probe went from 15 instructions to **7** (8 on the ARM7, which
   needs one `orr` to index the second LUT), verified by disassembly, strict
   frame diff byte-equal on SM64DS, Meteos and Mario & Luigi. On the device it
   lands at −0.3 % on sm64 and mlbis and −0.8 % on meteos: consistently on the
   right side of zero, inside the noise floor. Kept because it is strictly less
   code and less I-cache for one previously-unused register, not because it
   showed a win.
2. ~~**The LUT is 512 KB per CPU** — bigger is a net loss here, the clearest
   case in the audit of a technique reimplemented in a shape that inverts its
   benefit.~~ **Wrong. Measured on the device 2026-08-24 and reverted.**
   Shrinking to the technique's 8 KB is **1.6-2.7 % slower** on SM64DS and
   Mario & Luigi; 64 KB is roughly break-even; above 512 KB gains nothing.
   Numbers in `src/core/cpu/jit/README.md` (miss counts came from a temporary
   counter in `jit_h_lookup`, removed once the question was settled). Three
   errors in the reasoning above, worth keeping visible because they generalise
   to the rest of this document:

   - **The table is touched sparsely.** Only live blocks' entries are read, so
     the working set is the number of hot blocks, not the table size. "512 KB
     cannot live in cache" described the allocation, not the access pattern.
   - **Our miss path is far dearer than the technique's** (`call_pure` +
     `unordered_map::find` against a hand-written lookup), so capacity buys us
     more than footprint does. The technique's trade is not our trade.
   - **Translated code is 6-11 % of the process.** Dispatch wins are diluted
     roughly tenfold in frame time — which is the real lesson for items A-F
     below, all of which were sized by instruction count rather than by share
     of the frame.
3. **The pipeline-refill cycle charge is computed at run time**, with two
   `emit_fetch_cost9` expansions and internal branches, on the dispatch path.
   The technique does not do this at all. Same root cause as (A).
4. `LUT_BITS = 16` indexed as `(key >> 1) & 0xFFFF` wastes half the table for
   ARM keys (bit 0 of the index is always zero when the key is word-aligned).
   Moot once the table shrinks, but worth knowing.

The README already ranks this first. The audit agrees, and points at
(1) and (2) as the cheap parts of it — neither touches semantics.

### C. Every CPU switch leaves translated code

The technique's budget-exhausted path calls
`recompiler_cpu_next_action_...` — **in the arena**. Ours runs `exit_key`,
which stores all fifteen guest registers, merges and stores the flags, stores
the budget, and returns to `Scheduler::slice_next` in C++; re-entry through
`enter_light` reloads all fifteen, the flags, the budget and both table
pointers.

Our own measurement: `slice_next` is ~6 % of the run at ~300 cycles per slice,
7.3 k slices per frame in the 128-cycle lockstep default. That is **~2.2 M
cycles per frame** — larger than DraStic's *entire* CPU-emulation budget — spent
on entering and leaving.

Mitigations already exist (`--quantum 0` event-bound, the native `run_loop`),
and the README's "refill the budget in place from the poll stub" is the right
next step. Flagging it here because against the technique it is not a tuning
issue, it is a structural one: DraStic never leaves the arena to switch CPUs.

### D. Conditional execution always branches

The technique: "a following `MOVEQ` translates to a `CSEL`/predicated form —
the flags are simply already correct." ARM code is dense with conditional
execution.

We emit `b_cond_fwd(invert(cond))` around the body in every case
(`translate.cpp:1116`), plus a join branch when the instruction is not
simple-cost. On an in-order A55 a mispredicted conditional branch is ~8–11
cycles; a `csel` is one, and never mispredicts. For the common shapes —
conditional `MOV`, `ADD`, `MVN` with a register or immediate operand and no
flag write — the body is already one instruction and could be `csel`/`csinc`
directly.

This interacts with the `precharged` cycle path (`translate.cpp:1113`), which
exists precisely so the skipped path needs no code — that machinery makes the
`csel` form *easier*, not harder.

### E. Flag merging through a `bl` to an `mrs`/`msr` stub

`merge_keep_cv` and `merge_set_c` (`runtime.cpp:274`) are two `mrs`, a `tst`,
`ubfx`/`bfi`, and an `msr` — reached by `bl` and returning by `ret`. `mrs nzcv`
and `msr nzcv` are not cheap on an A55.

The liveness pass means this only fires when C or V is genuinely live after a
logical op, which keeps it off most paths — so this is a *check it, don't
assume it* item rather than a confirmed loss. `DS_PROF` plus the perf map
(`jit_stub_merge_keep_cv` is already named) will say how often it runs.

### F. LDM/STM

We fully inline the transfer, one `ldr`/`str` per register, where the technique
calls one of sixteen specialised straight-line helpers. Inline is better for
small transfers — no call at all.

The mismatch is the **page-straddle fallback**: our fast path requires the whole
transfer inside one 2 KB page (`add`/`eor`/`lsr`/`cbnz`, `translate.cpp:757`),
and anything that straddles goes to `emit_fallback` — a `call_full` spilling all
fifteen guest registers, then the *interpreter*. For a random 8-register POP the
straddle probability against a 2 KB page is ~6 %, and stacks straddle more often
than randomly. A hot `POP {r4-r11,pc}` sitting on a page boundary would be very
expensive. Worth instrumenting (`DS_JIT_HIST=1`) before deciding whether it
needs the technique's helper shape.

---

## Suggested order of work

Ranked by (expected win) ÷ (risk to the strict slice diff). The first two touch
no semantics at all.

1. ~~**Shrink the LUT and stop materialising its address.**~~ **Settled,
   2026-08-24** (see B1/B2). The address half is kept and neutral; the size
   half was wrong and is reverted.

   The lesson to carry into the rest of this list: **rank by share of the frame,
   not by instruction count.** Translated code is 6-11 % of the process, so a
   change that halves the cost of an operation inside it moves frame time by a
   fraction of a percent. Items 4 and 5 below (silent stores, GXFIFO) are the
   ones that remove *work* rather than instructions, and on that basis they
   should probably come first.
2. **`csel` for simple conditional instructions.** Removes a mispredictable
   branch from a very common shape. Cycle accounting is already pre-charged for
   exactly these cases. Same dilution caveat applies — measure before investing.
3. **Measure `DS_JIT_FASTCOST=1` on the device.** This is the decision point for
   (A), the biggest item. If exact data-cost accounting is costing more than a
   few percent, invest in resolving the cost statically where the page is known
   at translate time.
4. ~~**Silent-store elimination**~~ **Dropped — 0.1–1.4 % of slow accesses.**
   See §2.
5. ~~**The GXFIFO fast path.**~~ **Done 2026-08-24, generalised.** See §4.
6. **ITCM dispatch tables.** Largest new mechanism here. No longer gated on an
   invalidation story, since (4) turned out not to matter.
7. **Refill the budget in the poll stub** so a CPU switch does not leave the
   arena. Worth less than it looks: `entries` is ~620/frame under
   `--quantum 0`, against ~10,400 under the 128-cycle lockstep default.

### What is left, with measured shares

`perf` on the device, self time, JIT-relevant symbols only:

| candidate | sm64 | meteos | note |
|---|---|---|---|
| indirect-branch stubs | 1.39 % | 2.78 % | the cost is the `br` mispredict on a *shared* stub, not instruction count — proven by the LUT change, which removed 8 instructions for nothing. Reducing it means fewer dispatches or a per-site probe, not a shorter one |
| `memcmp` (2D palette/OAM compares) | 1.31 % | 2.70 % | not the recompiler, and never examined — bigger than most items here |
| fallback machinery | — | 2.07 % | `jit_h_fallback` + `call_full` + `exec_arm`, dominated by the ARM7 BIOS halt/IRQ path |
| `Scheduler::slice_next` | 1.33 % | 1.69 % | |
| `jit_stub_call_pure` | 0.90 % | 1.17 % | |
| `Timing::reset` + `update_cpu9` | — | 1.20 % | the JIT README claims what remains of `update_cpu9` is boot; 1.2 % over 900 frames says otherwise |

Note how flat that list is. Nothing left in the recompiler is worth more than
about 2 %, and the 3D rasteriser is ~35 % of the frame — see
[techniques/02](techniques/02-3d-software-rasteriser.md).

Everything in (1)–(3) can be measured against the existing SM64DS 300-frame
baseline in `src/core/cpu/jit/README.md` with the strict slice diff kept green.

(The `getenv`-per-2D-register-write in `Io::write` was fixed at the same time,
along with the same pattern in the VRAMCNT path.)

## Note on stale references

`docs/ARCHITECTURE.md` was removed because much of it rested on unmeasured
assumption, but eight source files still cite it by section (`cpu.h`,
`page_table.h`, `scheduler.h`, `cpu_mem.h`, `jit.h`, `kernels.h`, …). Those
comments should be rewritten to say what is actually true, or to point at the
measurements that replaced it.
