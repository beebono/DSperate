# Where the frame goes on the RG DS Plus — 2026-09-16

Measured on the `gpu-raster` branch while building the GPU 3D raster (its
own record is `docs/gpu-raster-workload-scoping.md` there, §8-9). What is
recorded here is the part that is about the software path and the emulation
thread, because that is where the frame turned out to be.

SDL frontend, `--dual-window`, clean runs (`DS_FRAME_STATS=1`, no profiler;
`DS_PROFILE=1` itself costs 1.5-2.7 ms a frame), both orders, median (p90).

## The 3D raster is never on the critical path

Under `DS_PROFILE=1` the emulation thread's `3d band wait` is 0.000-0.001 ms
on NSMB, Golden Sun, Etrian Odyssey and Spirit Tracks: the three band
workers cost 9-27 ms of worker time a frame and the emulation thread waits
for none of it. A GPU raster therefore cannot shorten the frame on this
device -- measured: it lengthened it, by the upload it puts on the emulation
thread -- and that work is stowed on its branch.

## What the emulation thread does, typical frame, software raster

| ms | NSMB | Golden Sun | Etrian | Spirit Tracks |
| --- | --- | --- | --- | --- |
| cpu arm9 | 6.7 | 7.5 | 2.4 | 5.9 |
| gx geometry (inline in the exact FIFO model) | 3.4 | 2.4 | 0.8 | 2.8 |
| cpu arm7 | 1.5 | 0.25 | 1.15 | 2.5 |
| dma | 1.5 | 2.9 | 0.3 | 0.9 |
| spu (one event per sample, ~540 a frame) | 0.9 | 0.4 | 0.65 | 0.5 |
| slice loop bookkeeping (~1,090 slices) | 0.7 | 0.7 | 0.3 | 0.7 |
| scanline hooks, minus the draws | 0.7 | 0.5 | 0.4 | 0.5 |
| gx vblank (swap, sort) | 0.4 | 0.2 | 0.2 | 0.2 |
| 2D on this thread, all rows | 0.7 | 0.5 | 0.4 | 0.7 |

The ten scopes that resolve the old "untimed" bucket are in `profile.h`
(`SCHED` .. `GPU_UPLOAD`); a per-event census (`[events]`, printed by the
scheduler's destructor) splits the `EVENTS` row by event id. (Golden Sun
and Spirit Tracks rows are from state scenes recorded before the loader fix
below and are ~1.5 ms light on `cpu arm7`.)

## The lever: the untimed geometry model

`emu.timing_oc` (`--timing-oc`) runs the geometry engine on its worker
instead of inline, and that is worth 3-4 ms a frame everywhere:

| scene | software, exact | software, untimed geometry |
| --- | --- | --- |
| NSMB (power-on, 2400 frames) | 18.7-18.9 (21.2) | **15.2-15.6 (16.9-17.1)** |
| Golden Sun (gsdd-phase2.dss) | 16.9-17.3 (18.4-18.9) | **14.3-14.6 (15.8-16.2)** |
| Etrian Odyssey (replay) | 9.3 (10.0) | **8.3 (9.4)** |
| Spirit Tracks (st-intro.dss) | 21.3 (23.3) | **17.0-17.2 (18.7-19.3)** |

It is an inexact model (Dragon Ball Origins' intro desyncs; anything pacing
itself on the FIFO stall or the swap wait sees different timing), so making
it the default is a decision, with a per-game exact override. Not made yet.

## What is left for the p90 tail

ARM9 emulation (6-7 ms on the two games that miss), Golden Sun's DMA (2.9),
the SPU's per-sample event (0.9 on NSMB; batching is a fidelity question),
the slice loop (0.7) and the scanline hooks (0.5-0.7).

## The state-loader bug this found

`st-intro.dss` and `gsdd-phase2.dss` are version-2 states written when the
scheduler had 20 events; the reader assumed 21 and shifted every event
parameter, so the ARM7's Timer1 -- the sound driver's tick -- fired once and
never rescheduled. Symptom: a loaded state that is silent and ~1.5 ms a frame
"smoother" than the same frames from power-on. Fixed (the chunk's size names
the count); `DS_DEBUG_STATE=1` dumps the armed events and the ARM7 timers
after a load.

## The accuracy tiers, and which gains are real (later the same day)

Every tier measured on the RG DS Plus, SDL frontend, `--dual-window`, clean
runs, software raster, both orders; emulation frame median (p90). The swap
column is the game's own 3D frame count over the run (`gx swap_buffers (new
list)` under `DS_PROFILE`), because **a tier can lower the frame time by
making the game draw fewer frames**, and the frame time alone cannot tell
that apart from a real saving.

| scene | exact | `--gx-worker` | `--cpu-oc` | `--cpu-uc` | `--timing-oc` |
| --- | --- | --- | --- | --- | --- |
| NSMB (2400 fr) | 18.7 (21.0) | 17.3 (19.0) | 16.4 (18.1) | 13.3 (17.3) | 15.6 (18.3) |
| ... swaps | 2260 | 2260 | 2295 | **1700** | 2260 |
| Golden Sun (400) | 17.2 (18.7) | 16.7 (18.6) | 17.2 (18.7) | 14.1 (16.0) | 14.4 (16.0) |
| ... swaps | 335 | 335 | 353 | **223** | 335 |
| Spirit Tracks (600) | 21.0 (23.0) | 19.8 (21.5) | 17.9 (19.8) | 15.3 (16.5) | 16.8 (18.4) |
| ... swaps | 600 | 600 | 600 | **400** | 600 |
| Etrian Odyssey | 9.1 (9.9) | 9.1 (10.0) | 8.7 (9.6) | 8.7 (9.7) | 8.6 (9.8) |

* **`--cpu-uc` (underclock) is not a smoothness gain.** It lets the guest
  CPUs get less done per emulated cycle, so games miss their own frame
  deadline on roughly every other frame and drop to an alternating 60/30 Hz
  cadence: NSMB -25 % of its frames, Golden Sun -33 %, Spirit Tracks -33 %.
  The emulator's frame time falls because there is less 3D to do. On Dragon
  Ball Origins' intro (which sets its rate by whether it finished before
  VBlank) it is visibly jumpy: 854 60 Hz gaps under exact vs 272 under the
  underclock. It is a last resort for a device that cannot reach 60 at all.
* **`--gx-worker` and `--cpu-oc` keep every frame** (`--cpu-oc`'s few extra
  swaps are the game running slightly ahead). Their gains are real but small.
* **`--timing-oc` keeps every frame on these three**, and is the largest real
  gain -- but breaks Dragon Ball Origins' intro, which paces on the FIFO
  stall that it removes. `--gx-worker` and `--cpu-uc` keep that stall and
  Dragon Ball's ordering.

### Also measured and rejected

* **Faster modelled FIFO drain** (`--gx-drain`, reverted): at most 0.4 ms;
  the guest's stall time is not where timing-oc's gain is.
* **Deferring the worker hand-off** (reverted): slower on NSMB and Spirit
  Tracks. The worker already runs on 89-97 % of frames under its controller.
* **Wider idle-loop skipping** (`DS_IDLE_SKIP=all`): skips almost nothing
  extra on these scenes. Spirit Tracks and NSMB have a genuine RAM-flag wait
  (a 3-instruction loop in ITCM) the analyser accepts, but the whole-machine
  rule vetoes it -- the ARM7 is busy, or a GXFIFO DMA is running.
* **What timing-oc actually removes** that the worker keeps: pricing each GX
  command on the emulation thread to keep the FIFO level exact (the "gx
  geometry" row, 1.4 ms on NSMB and 2.5 on Golden Sun with the worker vs 0.2
  under timing-oc), and GXFIFO DMA transfers fragmented by the level (1.29 M
  dispatch entries vs 0.96 M on Golden Sun).

### Two measurement traps

* The headless harness boots differently from the SDL frontend: Dragon Ball
  Origins runs its intro at 30 Hz headless (705 swaps) and at 60 Hz in SDL
  (1060) under the same exact model. A game's frame-rate behaviour must be
  checked in the frontend it will be played in.
* Worker mode as shipped (the per-frame shape controller) does not reproduce
  run to run; exactness checks in that path need `DS_GX_THREAD=2`.
