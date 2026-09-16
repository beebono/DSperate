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

## Phase 2 -- the device buffer follows the same number

`want.samples` from `buffer_size` rather than hard-coded: roughly a third of
the total, rounded to a power of two in 256..4096, with the queue target
taking the rest and clamped to clear it.

The thing to be careful of is that **SDL may not honour it.** On a
daemon-backed device the period is the daemon's, and `PIPEWIRE_LATENCY`
changed nothing when the native-PipeWire experiment tried it (see the
audio-path notes). So read `got.samples`, never assume `want.samples`, and
clamp the queue target against what came back. If the device refuses to move,
phase 2 degrades to reporting the floor honestly -- still worth having, but
the latency floor will not move on that device.

A device reopen is needed to change it, so the menu row applies this half in
`SettingsHost::commit()` -- the deferral point that already exists so that
stepping a row does not reopen the display on every press.

## Phase 3 -- `auto`, and the menu row

`audio.buffer_size = auto`, and an **AUDIO BUFFER** row on the Emu page:
`number("audio.buffer_size", "AUDIO BUFFER", 20, 200, 10, "auto", FlagLive,
Dep::None, ..., "auto", "AUTO", " MS")`. The table already has what this
needs -- `Setting::sentinel_value` is documented as taking `"auto"`, and
`number()` carries a display suffix, the same shape `FAST FORWARD SPEED`
uses for its `UNLIMITED`.

20 ms floor because that is about where a device can still be held: even at
512 samples the queue needs ~1.5 periods above it. 200 ms ceiling because
past there the drop latch is what you would feel, not the buffer. `Dep::None`
-- unlike `emu.limiter` and `emu.speed` this does not set the emulator's
pace, so netplay has no stake in it. Step 10 for round numbers, still six
times finer than the whole frames it replaces.

The controller is a slow outer loop above the rate control, not a
replacement for it:

- grow fast: +8 ms on a dry frame, or on the trim sitting at `DRC_CLAMP` for
  several seconds;
- shrink slowly: -8 ms after ~30 s in which the measured minimum stayed a
  comfortable margin clear;
- wide hysteresis, and inert under fast forward and the `over_` latch;
- **seed at launch**, per the phase 0 finding, so startup fill is not read as
  an underrun;
- `set_buffer_ms` resets `depth_` and `trim_`, so every step costs a
  re-converge transient. That is the argument for shrinking rarely.

`auto` moves the queue target only. The device buffer is sized once at open,
conservatively, because changing it means reopening the device.

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
