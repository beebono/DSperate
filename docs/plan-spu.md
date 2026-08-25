# Plan: the SPU, 11× DraStic for 4 % of the frame

## The number

Same measurement as [plan-cpu.md](plan-cpu.md) — SM64DS, 300 frames from
direct boot, RK3566.

| | ours | DraStic |
|---|---|---|
| SPU per frame | **0.37 ms** | **0.03 ms** |
| share of frame | 4.5 % | 1.0 % |
| ratio | | **11.4×** |

It is the worst ratio in the whole comparison — worse than the CPU's 5.2× —
but it is only 4.5 % of the frame, so the ceiling is about **0.34 ms**, or 4 %.
Worth doing, not worth doing first.

The per-unit figure is the useful one. At 32768 Hz and 60 fps the SPU produces
**546 output samples a frame**, each mixing **16 channels** — 8,738
channel-samples:

| | cycles per channel-sample |
|---|---|
| ours | **84** |
| DraStic | **6.8** |

Eighty-four host cycles to advance one channel by one sample and add it to a
mix is not a tuning problem. It is a structural one.

## Where it goes

`Spu::ev_mix` is a scheduler event that fires once per output sample and calls
`Spu::mix()`, which calls `run_channel(ch, TIMER_STEP)` sixteen times
([spu.cpp:331](../src/core/spu/spu.cpp)). Each `run_channel`:

- re-derives `fmt`, `psg` and `noise` from the control word,
- checks the enable bit,
- runs `while (c.timer >> 16)` with a **`switch (fmt)` inside the loop**,
- calls one of `next_pcm8` / `next_pcm16` / `next_adpcm` per sample,
- re-checks the enable bit at the bottom of the loop,
- returns one scaled sample.

So per output sample we pay 16 function calls, 16 format switches, 16 enable
re-checks, and a per-sample ADPCM step that re-reads the channel's decoder
state from memory each time. The scheduler also fires 546 events a frame just
to drive it.

**Nothing is amortised.** This is the same shape as the renderer's per-span
problem, and the same answer applies — except that here, unlike the span
batching, the destination is a flat output buffer with no ordering constraints
between samples, so batching is far less delicate.

## What DraStic does

Its symbol names give the design away:

```
spu_render_samples          spu_adpcm_decode_block
spu_clamp_block             spu_render_capture
spu_update_channel_settings spu_key_on
```

- **`spu_render_samples`** — plural. It renders a *block* of output samples per
  call, not one.
- **`spu_adpcm_decode_block`** — ADPCM is decoded a block at a time, with the
  decoder state (`index`, `predictor`) held in registers across the block
  instead of reloaded per sample.
- **`spu_clamp_block`** — the mix clamp is a separate pass over a block, which
  is the shape that vectorises.
- **`spu_update_channel_settings`** — the per-channel decode of the control
  word is hoisted to when the register is *written*, not evaluated per sample.
  That is our `fmt` / `psg` / `noise` derivation, and the enable check, moved
  off the hot path entirely.

Four of the six divergences are visible in the names alone.

## The plan

Each step is independently measurable, and the SPU has an easy correctness
bar: `--dump-audio file` writes the raw s16 stereo stream, so every change can
be diffed byte-for-byte against the baseline exactly as frames are.

1. **Hoist the per-sample control decode** (`spu_update_channel_settings`).
   Derive `fmt`, `psg`, `noise`, `vol_shift` and the enable state in
   `set_cnt` / `write`, store them in the channel, and delete the per-sample
   re-derivation and the switch. Smallest change on the list, touches nothing
   structural, and removes work from every one of the 8,738 channel-samples.

2. **Render a block instead of a sample.** Change `ev_mix` from one event per
   output sample to one per block of N (64 or 128 — measure), and give
   `run_channel` an output pointer and a count so its `while (c.timer >> 16)`
   loop fills a run. This is where the 84 cycles mostly are: 546 scheduler
   events and 8,738 calls a frame collapse to ~9 events and ~140 calls.

   The one ordering constraint to respect is that channel registers can be
   written mid-block by the guest. DraStic's block size is bounded by the same
   thing; the safe form is to clamp a block at the next scheduled SPU-register
   event, exactly as the CPU slice is clamped at the next scheduler deadline.

3. **Block ADPCM** (`spu_adpcm_decode_block`). With (2) in place, `next_adpcm`
   becomes a loop over a run with `index` and `predictor` in registers and the
   nibble fetch unrolled. This is the single hottest sample path in games that
   use ADPCM heavily, which on the DS is most of them.

4. **Vectorise the mix and clamp** (`spu_clamp_block`). Only after (2): with
   per-channel runs in buffers, the pan/accumulate/clamp becomes a flat NEON
   pass. Do this last and measure it on its own — the renderer work this
   session showed twice that materialising intermediate buffers can cost more
   than the vectorisation saves, and that trap applies here too.

## Ordering against the CPU work

The CPU plan is worth ~3.3 ms a frame; this is worth ~0.34 ms. Do the CPU work
first.

The exception is step (1), which is an afternoon's work with a byte-exact
audio diff and no structural risk. It is worth taking out of order simply
because it is cheap.

## What success looks like

- `spu` stage time per frame, `DS_PROFILE=1`, direct-boot SM64DS 300 frames,
  against the 0.37 ms baseline.
- Cycles per channel-sample against 84 now, 6.8 for DraStic.
- `--dump-audio` byte-identical at every step, or a stated and understood
  reason why not.
