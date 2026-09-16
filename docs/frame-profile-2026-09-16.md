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
