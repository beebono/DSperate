# Phantom Hourglass opening: console performance investigation

Measured on 2026-10-20 with the graphical SDL frontend, the first 2,000
frames of the European cartridge (`AZEP`), both physical 1024x768 panels,
audio enabled, no frameskip, and the CPU's **exact** timing model. The
console has four Cortex-A55 cores, a 1.992 GHz maximum CPU frequency, a
Rockchip RGA, and a shared 32 MiB CMA pool. It runs a Wayland session.

No BIOS/firmware dumps were available: these runs use FreeBIOS and generated
firmware. “Exact” here names the CPU cycle model; it does not establish
Nintendo BIOS timing or pixel equality with a reference emulator.

## Cause and fix

Fullscreen Wayland configuration enlarges each window from 512x384 to
1024x768. The frontend requested five XRGB8888 dma-bufs for each window:
30 MiB across the two panels, before the compositor, RGA source buffers,
and buffers still held during resize. Allocation failed during the resize.
One window lost RGA presentation, the other deliberately followed it, and
one scanout path also fell back to SDL's window surface. The log therefore
announced RGA at startup even though gameplay used CPU scaling and copying.

`DmabufOut::open` now targets at most 10 MiB of buffers per window, with a
minimum of two buffers. This selects three buffers per 1024x768 panel
(18 MiB total). Smaller windows retain the requested run-ahead. The desired
count is kept separately from the actual count, so resizing back down can
restore it. An allocation failure after at least two successful allocations
retains that smaller ring instead of abandoning the presentation path.
Verbose logging reports the actual dimensions, count and memory use.

An earlier two-buffer experiment kept RGA alive but introduced frequent
waits for compositor releases. Three buffers provide the additional slack
while still fitting the console's shared pool. There is no change to guest
CPU, DMA, geometry, or cartridge timing, and no title-specific shortcut.

## Measurements

Unprofiled runs used `DS_VERBOSE=1 DS_FRAME_STATS=1 DS_FPS=1` and:

```sh
dsperate 'game.nds' --timing exact --frames 2000 --frameskip 0 --no-vsync
```

`game.nds` is the user's cartridge dump. Presentation remains subject to
Wayland buffer release even with `--no-vsync`. The DS's native frame rate
is approximately 59.826 Hz, reported as 59.8 fps at full speed. “Work” is
emulation plus the frontend's submission/wait time, excluding frame pacing;
the presentation worker overlaps emulation. The frame-budget statistic uses
16.715 ms. An audio “dry” frame means the output queue was empty when checked;
it is not a count of individual audible clicks.

| Run, 2,000 frames | Mean work, ms | p95 work, ms | Work over budget | Audio dry frames |
| --- | ---: | ---: | ---: | ---: |
| Original, exact, run 1 | 14.867 | 19.405 | 511 | 166 |
| Original, exact, run 2 | 15.731 | 20.229 | 590 | 253 |
| Two buffers/panel, exact (experiment) | 12.829 | 17.906 | 217 | 30 |
| Three buffers/panel, exact, run 1 | 10.463 | 16.588 | 93 | 15 |
| Three buffers/panel, exact, final repeat | 10.104 | 14.741 | 20 | 3 |
| Three buffers/panel, exact, idle skip `all` (experiment) | 10.369 | 15.023 | 37 | 6 |
| Three buffers/panel, exact, `DS_JIT_WARM=2` (experiment) | 10.030 | 14.579 | 20 | 6 |
| Three buffers/panel, fast timing (comparison) | 9.233 | 13.404 | 17 | 3 |

The final repeat used the saved exact timing setting, with no explicit timing
CLI override and no idle-skip or JIT-warmup environment overrides. All
60-frame FPS windows were 59.3–59.8 fps, with the last ten at 59.8. Of its
20 over-budget work frames, 18 occurred in the first 300 frames, two in the
remaining 1,700, and none in the final 400. Both panels retained three dma-bufs
and RGA presentation, with no allocation failures. The exact rendering
cadence again matched the original on all 2,000 frames.

The original build sustained roughly 52–58 fps late in the opening. With
three buffers the sustained sections generally hold 59.8 fps. Individual
cold-loading/JIT translation hitches and occasional scheduling/presentation
outliers remain; these results do not claim every frame meets its deadline.
The user confirmed both panels display correctly and reported substantially
steadier playback after the change. Repeated runs vary, so the optional
idle/warmup experiments should not be treated as isolated causal wins.

`DS_CADENCE_LOG` was compared frame by frame: original exact, fixed exact,
fixed exact with idle skip `all`, and fixed exact with `DS_JIT_WARM=2` all
produced the same swap count, polygon count, and presentation flag on every
one of the 2,000 frames (1,823 swaps). This checks that the improvement did
not simply reduce the game's geometry cadence; it is not a framebuffer hash
or an instruction-by-instruction comparison. Fast timing differed on 1,771
frames and produced 1,841 swaps. Fast is therefore a different workload,
not a substitute for validating the exact model. Exact remains the selected
console timing setting; the optional idle and JIT warmup settings are not
made new defaults.

## Profiling and verification

Light `DS_PROFILE=1` profiling identified CPU emulation and cold translation
bursts on the main thread, plus substantial worker time in scaled output.
The original exact run's worst frame was around 34 ms; the p99 group spent
about 5.9 ms in translation. A console `perf record -e cycles:u -F 999`
run also identified CPU-side VRAM remapping, memory operations, software
rasterization and output scaling. Profiling perturbs this small frame budget;
the table above excludes both built-in profiling and perf sampling.

Three profiler-reporting bugs were corrected while investigating:

- New JIT substages had no corresponding JSON keys, shifting later labels
  and producing repeated `(null)` keys. The key list now covers every stage,
  with a compile-time count check. A 300-frame graphical profiling run
  verified the JSON has valid stage keys and its mean matches the frame summary.
- JSON `ms.mean` reported the middle fifth's mean instead of the whole run.
- Individual worst-frame `untimed` values double-counted nested stages.

The ARM64 graphical executable was built locally and deployed with SHA-256
verification. The final frontend also builds natively on x86-64. The
`page_table`, `scheduler`, `gpu`, and `io` host tests pass, including the
existing lazy-render/mid-frame VRAM-remap coverage. The graphical runs on
the console establish actual emulation, rather than only a successful
transfer or process launch.

The local build lacks Vulkan headers, so GPU 3D is unavailable in this
artifact. The measured fix uses RGA presentation and the existing software
3D renderer. No system settings, kernel memory reservations, services,
repository workflows, or online repository state were changed.
