# Nearest-neighbour scaling with the Rockchip RGA presenter

With `video.linear = false` (the default), the CPU resizes each 256x192 DS
view to its displayed dimensions using nearest neighbours. RGA then copies
it at 1:1. Pixel colours are copied directly, with no interpolation, at
integer and fractional scales alike. Small insets also use nearest neighbours.

Integer scaling is optional: `video.integer_scale = off` fills the normal
aspect-preserving layout, `under` letterboxes at a whole-number scale, and
`over` crops. For example, a 640x480 panel can show a full 640x480 image at
2.5x with nearest neighbours, rather than being restricted to 512x384 at 2x.
The shared layout and touch mapping retain their existing rectangles.
`video.linear = true` / `--linear` restores RGA's bilinear scaling; the existing
BILINEAR menu toggle is available on RGA.

These are the emulator's existing options; no additional integer-scaling or
filter setting is introduced. The INTEGER SCALE row is disabled when no
offloaded presenter is actually running, regardless of the config's request,
with the message `NOT AVAILABLE WITH THE CURRENT DISPLAY MODE`. Reopening the
display for a filter change preserves the selected presenter mode and does
not write or change `video.gpu_present`.

The running console's `rockchip-rga` V4L2 driver exposes horizontal flip,
vertical flip, rotation and background colour controls, but no filter
selector. A vendor `librga` is installed without its `/dev/rga` endpoint.
This is a limitation of the available driver interface, not a claim that
the silicon cannot perform nearest-neighbour scaling. The
[Linux driver disables scaling when crop and compose sizes match](https://codebrowser.dev/linux/linux/drivers/media/platform/rockchip/rga/rga-hw.c.html#259),
which enables the userspace workaround without a kernel change.

`frontend/video/nearest_scale.cpp` implements centre-sampled nearest-neighbour
resampling. It reuses repeated source rows; uniform integer scales retain
fast paths, including NEON for 2x and 4x. `video/rga_source.h` owns one cached
dma-buf per presenter, reused after each synchronous RGA job. The allocation
stride is padded to the driver's alignment while the crop uses the exact
view size, including odd widths. Cropping and fading insets keep their
existing composition paths. Guest CPU timing is unchanged.

## Validation

The `nearest_scale` tests check every output pixel against centre sampling
with random source colours, padded pitches and guard rows. Sizes cover
integer factors 1–5, 640x480 (2.5x), 853x640, a 213x160 inset, 257x193,
600x1000 and 1x1. Tests also check that ordinary 640x480 placement fills the
panel and that explicit integer placement still works. `layout` and
`nearest_scale` pass natively; the pixel tests also pass under AArch64 QEMU
with NEON enabled.

An on-console probe drives the actual RGA presenter and compares every
output RGB pixel, including borders, against centre-sampled nearest
neighbours. All six cases have zero mismatches:

| Output | View | Case |
| --- | --- | --- |
| 1024x768 | 1024x768 | 4x full-screen |
| 640x480 | 640x480 | 2.5x full-screen |
| 640x480 | 768x576 | cropped 3x |
| 1024x768 | 213x159 | shrinking inset with odd dimensions |
| 1024x768 | 853x640 | fractional scaling with odd width |
| 1024x768 | 641x481 | clipped fractional scaling |

The device probe also changes the source to bilinear and back, exercising
V4L2 stream reconfiguration. Graphical game validation uses Picross DS
(`AXPP`) on the console's physical screens with exact CPU timing, audio and
no frameskip. Screenshot captures are separate from performance measurements:
writing raw panel buffers to storage can stall the presenter.

A 2,000-frame Picross replay navigates from boot into Easy Mode's first
puzzle and dismisses the hints prompt. At dual-panel 1024x768, with nearest
filtering, exact timing, audio enabled and frameskip disabled, the game reports
59.8 fps (100%). Mean presentation time is 8.016 ms; mean emulation work is
4.188 ms. Three emulation-work frames exceed the 16.715 ms budget, all within
the first 200 frames. Audio records one dry event, no dropped samples and no
ring overruns. These are measurements of this run, not a guarantee of zero
startup hitches or results for other games.

A separate graphical capture uses a single-panel dominant layout with
639x479 and 383x287 views (approximately 2.5x and 1.5x). The finished RGA
buffer shows the playable puzzle's grid and text, and the diagnostic log
confirms matching source/destination dimensions for both views. This run
includes screenshot and raw-buffer writes and is excluded from the timing
figures above. Layouts with differently sized views also reconfigure the
single staging source between views, adding overhead compared with two
equally sized panels.
