# Sizing the audio buffer -- scoping

2026-09-15. `dsi-todo` item 6: dynamic audio buffer sizing, with static
tuning through a config key. What the buffer actually is, what it costs, and
what an automatic one has to steer on.

## What the buffer is, and why it is two numbers

Output latency is the sum of two buffers, and before this work only one of
them had a name.

| | before | what it is |
|---|--------|------------|
| the queue | `audio.latency_frames = 3` (50.1 ms) | what we have pushed and SDL has not handed the device yet; the rate control holds it here |
| the device's period | `want.samples = 2048` (42.7 ms at 48 kHz) | hard-coded, undocumented, never measured |

**~93 ms**, then, not the 50 the one knob suggested. And the second number is
2.55 DS frames, which makes it the floor under the first: the device drains
in whole periods of its own, so a queue target under one period is a depth
nothing can hold. `latency_frames = 1` was accepted by the setter and was
never reachable.

They also could not be compared. One was a count of frames, the other a
count of samples; they only become the same kind of thing in milliseconds,
which is what the key is now.

## Phase 0 -- measure first -- **DONE (2026-09-15)**

There was no underrun signal at all. A queued SDL device that runs dry does
not report anything -- it plays silence and carries on -- and the only
existing counters (`aq_min`, `aq_under1`) were gated behind a netplay
session. Nothing could steer on that.

`Audio::Stats` now counts dry frames, depth extremes and drops, gathered at
the point in `Audio::push` where the depth is already measured so there is no
extra SDL call for it. A dry frame -- the queue empty when the frame looked
at it -- is the closest thing to an underrun event we can see, and it is what
an automatic size has to grow on. Alongside it: the SPU ring counts what it
overwrites unheard (a plain member, absent from `sync_state`, so no state
format change), `device_buffer_frames()` reports the device's own period in
frames, `DS_AUDIO_QUEUE` carries target/dry/input-rate beside the depth, and
the run summary is no longer netplay-only.

Two findings, immediately:

- **The device buffer really is 2.55 frames** at the 2048 samples we ask for,
  confirming it as the floor.
- **8 of 11 dry frames in a 900-frame run were the queue filling from empty
  at startup.** Consequence for phase 3: an automatic size must seed the
  buffer at launch rather than read startup fill as an underrun, or it will
  grow forever on a fault that is over by the time it reacts.

And one that turned out to matter more than the buffer work: the trim the
rate control was holding was **correcting our own sample rate**, not the
host's. See "The rate that was not a rate" in `docs/frame-pacing-scoping.md`;
it was worth ~7 ms of latency the buffer could not otherwise account for.

## Phase 1 -- the key, in milliseconds -- **DONE (2026-09-15)**

`audio.buffer_size`, milliseconds, default 50 (what `latency_frames = 3`
came to, so nobody's sound changes). `--audio-buffer MS` alongside it, in the
CLI-override list, live-settable. `latency_frames` is still read when
`buffer_size` is unset and converted at 16.715 ms a frame, so an ini or a
per-game file written before the rename keeps its setting rather than
silently reverting.

`target_frames_` is a double now. At whole frames the smallest expressible
step is 16.7 ms, which is most of the interesting range once the device
buffer is sized too -- a millisecond-valued key on an integer target would
have been a lie in the third digit.

The file accepts 10..400 ms; a menu row will offer less than that. Verified
on the host: default lands on exactly 3.00 frames, 30 -> 1.79, 120 -> 7.18, 5
clamps to the 10 ms floor, `latency_frames = 6` -> 100.3 ms, and
`buffer_size` wins when both are set.

## What the measurements changed

Phases 2 and 3 were scoped on the assumption that a deeper buffer reduces
popping. **It does not, except in one narrow case**, and that reshaped both.

Controlled A/Bs, identical workload each time (RG DS Plus, performance
governor, wayland display path), dry frames per 1800:

| title | 50 ms | 100 ms | 150 ms | 200 ms |
|-------|-------|--------|--------|--------|
| sm64 replay | 302 | 345 | 342 | 266 |
| Spirit Tracks (no input, deterministic) | 191 | 167 | 205 | 186 |

No trend in either. A buffer absorbs jitter around a *sustainable average*;
it cannot absorb a deficit. At 93 % of real time the queue drains at 7 % of
real time whatever its depth, and refilling needs to run *above* real time --
which **the frame limiter forbids**. At the limiter production equals
consumption exactly, so the only thing that can add depth is the rate
control's 0.5 % of trim, 0.083 ms a frame. That is why a 200 ms target never
got past 124 ms of real depth, and why raising a target the machine cannot
fill pins the trim at its clamp and detunes the output half a percent flat
(~8 cents) for as long as it lasts.

So the triage inverts what this document assumed:

- **high `dry` = the machine is not keeping up.** The fix is frameskip, CPU
  tuning or the governor. Not buffer size.
- **buffer depth answers isolated hitches** on a machine that otherwise holds
  100 %. sm64 with the governor fixed: `dry 1/1800`.

Two traps voided a day of measurement before this was clear, both on the RG
DS Plus: the `ondemand` governor (mean emu 13.26 ms against 7.34 on
`performance`, and dry 174-272 against 1), and SDL falling back to
`software renderer, offscreen driver` over ssh, which costs 3.8-5.5 ms of
present and manufactures the very deficit being measured. Grep the `video:`
line and check the governor before believing anything from that device.

## Phase 2 -- the device buffer -- **DROPPED**

Sizing `want.samples` from the same number was scoped to lower the latency
floor and to give a weak device a larger device period. The measurements
above remove the reason for both: depth is not what is failing, and the
period is not where the gaps come from. `got.samples` is reported at open
(2.55 frames on every device tested) and clamps nothing else. Revisit only
with evidence of a fault *downstream* of the queue -- popping with `dry` at
zero, which nothing has shown yet.

## Phase 3 -- `auto`, and the menu row -- **DONE (2026-09-15)**

`audio.buffer_size = auto` is the default, and an **AUDIO BUFFER** row sits on
the Emu page (20..200 ms, step 10, `auto` sentinel, `FlagLive`, `Dep::None`).

The controller is a slow outer loop above the rate control, and the gate is
the part that matters:

- a decision every 300 frames (~5 s);
- **grow only when the machine is keeping up** -- 90 % of frames inside their
  budget -- and it still ran dry. 90 rather than 100 because adaptive
  frameskip and a vsync beat each put the odd frame over, and those are
  exactly the hitches depth is for;
- when it is behind, *leave the target alone*. Growing into a deficit is the
  failure mode this whole phase exists to avoid;
- +8 ms a step to 150 ms, -8 ms after six clean windows (~30 s) to a 30 ms
  floor;
- **inject the step as silence** rather than waiting for the trim to fill it.
  The limiter leaves no headroom, so without this a step takes ~20 s to
  materialise with the trim on its clamp throughout. The silence is paid at a
  moment the queue has just run dry, so it lands in a gap that already
  existed;
- 60 frames of settling after every step and at startup, because the queue
  fills from empty at launch and that would otherwise read as a run of
  underruns -- 8 of the first 11 dry frames in a 900-frame run were exactly
  that.

`DS_AUDIO_AUTO=1` logs every decision. Verified on the RG DS Plus in both
regimes:

    deficit (Spirit Tracks)   50 ms, 1 dry, 282/300 on time -> grow
                              58 ms, 44 dry, 148/300 on time -> behind, leaving it
                              58 ms, 68 dry, 145/300 on time -> behind, leaving it
    healthy (sm64 replay)     50 ms, 1 dry, 298/300 on time -> grow
                              58 ms, 0 dry, 300/300 on time -> clean

The deficit case is the one to read: 68 underruns in a window and it still
refuses to grow, because growing would not have helped and would have
detuned the sound to prove it.

## Acceptance

The measurement `frame-pacing-scoping.md` asks for, which is still owed on a
device: 30 minutes on the RG DS *and* the A30 -- the thin-margin case -- at
100 %, 90 % and 200 %, with no dry frames and a steady depth.

Rig gotchas, from the audio-path notes: state the backend and whether the
device opened at 32768 or at its native rate; `--no-audio` is not a fair
baseline for a shipped build; and append to a *copy* of the device ini rather
than a bare `--config /tmp/x.cfg`, which drops `dual_window` and reads ~0.9 ms
faster.

A DSi session needs all four BIOS dumps, not just the DSi pair, or it wedges
a few frames in -- gated since 2026-09-15, but worth knowing when setting up
a rig.
