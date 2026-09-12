# Decoupling frame pacing from audio -- scoping

2026-09-12. What it takes to stop using the audio queue as the emulator's
clock and pace on a real, controllable frame limiter instead, and what has to
be built on the audio side so that sound survives the change.

## What we do today

The pacer is three lines at the bottom of the frame loop in
`src/frontend/sdl/main.cpp:3686-3712`, and it picks one of three clocks per
frame:

| condition | clock |
|-----------|-------|
| fast forward with `ff_speed = 0` | none -- run flat out |
| audio device open and not fast-forwarding | `Audio::pace()` -- the queue depth |
| everything else (`--no-audio`, a stalled device, capped fast forward) | `next_frame` wall clock |

`Audio::pace()` (`src/frontend/sdl/audio.cpp:145`) sleeps in 1 ms steps while
`SDL_GetQueuedAudioSize()` is above `TARGET_FRAMES * frame_bytes_` (3 frames,
~50 ms). A frame of emulation always produces the same 547-odd SPU frames, so
holding the queue at a fixed depth holds the emulator at exactly the rate the
*sound card* consumes samples. That is the whole mechanism: the DAC is the
oscillator and the emulator is slaved to it.

The wall-clock path already exists and is a working frame limiter in
miniature: `frame_ns = 1e9 * CYCLES_PER_FRAME / ARM9_CLOCK_HZ` (59.8261 Hz),
`next_frame` accumulates, and it deliberately carries up to one frame of debt
forward so an alternating heavy/light pair (Spirit Tracks' intro) is on time
over the pair. It is entered when audio is off or the device is dead, and it
is reset to "now" on unpause, on state load, and whenever the audio clock is
in charge -- see the `next_frame = SDL_GetPerformanceCounter()` sites at
`main.cpp:2359`, `:2940`, `:3075`, `:3260`, `:3695`.

Three consumers read the pacing decision and would have to keep working:

1. **Adaptive frameskip** (`fs_debt_ms`, `main.cpp:3671`) measures debt against
   `frame_budget_ms`, which is `frame_ns` -- already limiter-relative, not
   audio-relative. It survives untouched.
2. **`DS_WIFI_SLICE`** (`main.cpp:3367`) reads `next_frame` directly to spread
   emulation across the frame period, and comments that it is only meaningful
   "when the audio queue is the clock" resets it. A real limiter makes this
   path *more* correct, not less.
3. **Fast forward** divides `frame_ns` by `ff_speed`, and `audio.push(nds,
   fast)` drops whole frames of audio once the queue is at target. Under a
   limiter the drop condition should follow the limiter's speed, not a fixed
   depth.

## Why the audio clock is worth leaving

- **The clock is not ours.** The device rate is the daemon's (48 kHz on the RG
  DS via PipeWire), the DS rate is 59.8261 Hz, and the panel is 60 Hz. Today
  the emulator is locked to the first of those three, which is the one with no
  relation to either of the others.
- **It cannot be controlled.** There is no "run at 60 Hz", no "run at 50 %",
  no "uncapped without fast-forward's frame dropping", no per-game speed. The
  only knobs are `ff_speed` (integer multiple, fast-forward only) and
  `TARGET_FRAMES` (a compile-time constant).
- **It fails in ways we have had to paper over.** `stalled_`,
  `STALL_RETRY_MS`, the 100 ms bounded wait and the "device is not consuming
  samples" message all exist because a queue-as-clock hangs when the queue
  stops draining. That entire failure mode disappears with a wall clock.
- **Latency is fixed at ~50 ms** of slack, because the depth *is* the clock:
  lowering it to cut latency also makes the pacer twitchy.
- **It collides with vsync.** The present blocks on the panel (60 Hz) while
  the pacer holds 48 kHz/547; the beat between them is part of what the dual
  window work already had to absorb.

## What has to be built

### Phase 1 -- make the wall clock the only pacer -- **DONE (2026-09-12, unmerged on `wifi-emu`)**

`src/frontend/sdl/pacer.h` is the limiter; `Audio::pace()` and the whole
stall machinery are gone; `next_frame` and its four reset sites are now
`Pacer::reset()`, and `DS_WIFI_SLICE` reads `Pacer::next()`. As a stopgap
until phase 2, `Audio::push` drops whole frames once the queue drifts past
`MAX_FRAMES` (6) and keeps dropping until it is back at `TARGET_FRAMES`.

Measured on the host, Meteos scene, 1800 frames: 59.8 fps held throughout
with audio on, queue oscillating between 0.9 and 2.2 frames and never
tripping the drop; `DS_WIFI_SLICE=1` under `--lan-host` holds 59.8 too.

Small and mechanical: always take the `on_the_clock` branch (except uncapped
fast forward), and stop calling `Audio::pace()`. Replace the sleep with a
proper limiter primitive rather than `SDL_Delay(ms)`:

- a `Pacer` type (new `src/frontend/sdl/pacer.h`, ~100 lines) holding
  `target_ns`, `next_frame`, the debt rule already in `main.cpp:3706`, and a
  `reset()` for the unpause/load/speed-change sites listed above;
- sleep to `next_frame - spin_margin` with `nanosleep`, then spin the last
  ~0.5 ms. `SDL_Delay` rounds to whole milliseconds and, on a 16.7 ms frame,
  a 1 ms rounding error is 6 % of the period -- fine when the audio queue was
  absorbing it, not fine when this *is* the clock. (`rt_thread.h` is already
  there for the RT priority this wants.)

After phase 1 the emulator is paced but the sound is wrong: the queue now
drifts, because nothing holds it at a depth any more. Phase 2 is not optional.

### Phase 2 -- make audio follow the pacer (the real work)

The device consumes 48000 samples/s of wall time; the emulator now produces
`547 * limiter_rate` per second of wall time. At 59.8261 Hz nominal these
differ by whatever the sound card's crystal is actually doing -- typically
tens to hundreds of ppm -- so the queue walks to empty or to the ceiling over
minutes. Options, cheapest first:

1. **Dynamic rate control.** Measure the queue depth each frame, and nudge the
   resampler's step (`step` in `Audio::push`, `audio.cpp:98`) by a fraction of
   a percent to steer the depth back to target. This is the standard fix, it
   is ~20 lines on top of the linear resampler we already have, and it is
   inaudible at the +/-0.5 % it needs. It also works unchanged at non-1.0
   limiter speeds: 90 % speed simply means a different steady-state step.
   **Recommended.** The existing two-tap linear interpolator is already the
   quality floor here; if the pitch wobble is audible on a hard case, the
   upgrade is a windowed-sinc or cubic step, not a different control scheme.
2. **Insert/drop samples** at the queue boundary. Cheaper, audibly worse
   (clicks on a sustained tone). Only worth it as a fallback for the tier
   where a resampler is too expensive -- which, given phase 0 measured the
   resampler at well under the ~1 ms the daemon's own conversion cost, is
   probably no tier we ship.
3. **Callback-driven output with a ring.** Correct, but it moves audio off the
   emulation thread and breaks the "the whole frontend is single-threaded"
   property the header explicitly claims. Out of scope.

Also in phase 2:

- `TARGET_FRAMES` becomes a knob (`audio.latency_frames`, default 3) since it
  is now purely a latency/underrun tradeoff and no longer a pacing constant.
- The whole stall machinery (`stalled_`, `announced_`, `stall_mark_`,
  `STALLED_FRAMES`, `STALL_RETRY_MS`, the 100 ms probe) can be deleted. A dead
  device becomes "the queue grows; drop"; it costs nothing and cannot hang the
  emulator, because the emulator no longer asks it anything.
- `Audio::push(nds, drop)`'s drop threshold has to key off the limiter speed
  rather than a fixed `TARGET_FRAMES`, or every non-1.0 speed leaks the queue.
- One existing inconsistency surfaces here: `Spu::output_rate()` can be 47605
  (`MIX_PERIOD_47K`) but `push()` hard-codes `Spu::SAMPLE_RATE` as the
  resampler input and `frame_bytes_` assumes `rate_/60`. Under the audio clock
  a wrong `frame_bytes_` just shifts the target depth harmlessly; under DRC it
  biases the controller. Worth fixing in the same pass -- read `output_rate()`
  and derive `frame_bytes_` from `frame_ns`, not from 60.

### Phase 3 -- the controls

The point of the exercise. New keys, following the existing `settings.cpp`
table pattern (`number()` / `choice()`, `FlagLive`, `Dep::NetSession`):

| key | menu row | notes |
|-----|----------|-------|
| `emu.speed` | GAME SPEED | percent, 25..400 default 100; `FlagLive`, `Dep::NetSession` (a peer keeps time under netplay -- same rule that already gates frameskip and fast forward) |
| `emu.limiter` | FRAME LIMITER | `auto` (DS rate) / `off` / a number in Hz. `off` is *not* fast forward: it runs uncapped but still presents and still plays audio (drop-driven) |
| `emu.limiter_sync` | SYNC TO DISPLAY | pace to the panel's measured refresh instead of 59.8261, for a 60 Hz panel where the 0.29 % beat shows as a periodic stutter |
| `audio.latency_frames` | AUDIO LATENCY | 1..8, default 3 |

`--speed N` / `--limiter X` CLI flags alongside them, and the same keys added
to the CLI-overrides list at `main.cpp:1056`. `emu.speed` and `ff_speed` then
compose as a single `target_ns = frame_ns / (speed * ff_multiplier)`, which
also gives fast forward a fractional cap for the first time.

`emu.limiter_sync` is the one with a real unknown: it needs a refresh estimate
per tier, and the DRM/disp/fbdev presenters each learn it differently
(`display_disp.cpp:795` already has a "FBIOPAN_DISPLAY does not wait for
vsync" fallback). Ship it behind the knob, default off.

## What this does not change

- Netplay. `net_live` already forbids every knob that lets the emulator set
  its own pace; `emu.speed` and `emu.limiter` join that list via
  `Dep::NetSession` and the guard at `main.cpp:2437`.
- Adaptive frameskip, which was never audio-relative.
- The headless frontend (`src/frontend/headless`), which does not pace at all.
- Determinism. Nothing here touches the core; replays and scene hashes are
  unaffected, which also means the existing `.rec` replay gate is a free
  regression test for the whole change.

## Risk and effort

| phase | effort | risk |
|-------|--------|------|
| 1 pacer | ~1 day | low -- the wall-clock path already ships and is exercised by `--no-audio` |
| 2 audio DRC | ~2-3 days | **the risk lives here.** A badly tuned controller is audible; an untuned one underruns |
| 3 controls | ~1 day | low, table-driven |

Total ~1 week including device time. Phase 2 is the one that needs real
measurement rather than review: the acceptance test is a 30-minute run on the
RG DS and on the A30 with the queue depth logged per frame (the `aq_min` /
`aq_under1` counters at `main.cpp:3684` already exist for the LAN stats and
can be promoted to a `DS_AUDIO_QUEUE` series), showing no underrun and a
steady-state depth inside +/-0.5 frames -- at 100 %, at 90 %, and at 200 %.

## Recommendation

Do it, in that order, and do not ship phase 1 without phase 2. The audio clock
is not a bad clock -- it is an accidentally *good* one, which is why it has
lasted -- but it is unownable, it caps latency at 50 ms, and it is the reason
there is no speed control. Dynamic rate control is the well-trodden exchange:
we give the sound card a resampler it can steer, and we get the clock back.
