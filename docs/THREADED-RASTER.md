# Banded 3D rasterising

The 3D span stage is the largest single cost in a gameplay frame (~33% on the
RK3566), and it was the last major stage still running entirely on the
emulation thread. It is now split across horizontal bands, one worker per band.

DraStic does the same thing and it is where its wall-clock advantage partly
comes from: profiling it on the device shows two `render_polygon_*` worker
threads plus a 2D scanline thread, ~33% of its CPU off the main thread, and
that is with its own `threaded_3d` option *off* (that option controls whether
the 3D **frame** is decoupled, not whether bin raster is parallel).

## Measured

900 replayed frames, headless CLI, RK3566 (4x Cortex-A55):

| scene | 1 band | 2 | 3 | 4 |
| --- | --- | --- | --- | --- |
| SM64DS | 17590 ms | 14621 (-17%) | 13871 (-21%) | 13248 (**-25%**) |
| Mario & Luigi | 11973 ms | 10180 (-15%) | 9655 (-19%) | 9385 (-22%) |
| Meteos | 9274 ms | 8482 (-9%) | 8368 (-10%) | 8228 (-11%) |

SM64DS goes from 19.5 ms to 14.7 ms per frame, i.e. across the 16.67 ms line.
Meteos gains least because it is the least 3D-heavy of the three.

The default is **3 bands**, not 4: the fourth band takes the core the SDL
frontend and the audio thread need, and 3 already collects most of the win.
`DS_R3D_THREADS=n` overrides it; `0` or `1` disables banding entirely and is
the A/B reference. At the shipped default:

| scene | 1 band | default (3) |
| --- | --- | --- |
| SM64DS | 17623 ms | 13860 (-21%) |
| Mario & Luigi | 11924 ms | 9717 (-19%) |
| Meteos | 9243 ms | 8359 (-10%) |

### Do not gate banding on polygon count

The first version skipped banding below 24 polygons, reasoning that a short
list is cheaper on one thread. That silently gave back 12% of the win on
SM64DS (default 15559 ms against 13865 ms when 3 bands were forced), because
polygon *count* says nothing about raster cost: a handful of large polygons -
a skybox, a full-screen quad - is a full frame of spans. Area would be the
right measure and is not worth computing, so the cutoff is now 2 polygons.
If you ever reintroduce a heuristic here, A/B it against forced `=3` on a
3D-heavy scene, not against the single-threaded path.

## Why bands are safe

A band owns its line ring, its active polygon set and its edge cursors, and
writes only its own output lines. Nothing mutable is shared. Two properties
make this work:

* `Slope::setup` takes the line to position at and computes the edge state
  directly from it (`dx += (y_ - y0) * increment`), so a band can enter a
  polygon that began above its first line without walking the lines between.
  `seed_active` uses this to build the active set at the band's first line.
* The active set is seeded in **list order**, which the blending rules depend
  on, and band boundaries are fixed for a given band count. The split is
  therefore deterministic: output does not depend on thread scheduling.

The final pass of a line reads its two neighbours (edge marking), so a band
rasterises one line above the range it emits; the line below comes from its own
loop. At the top and bottom of the screen the border rows stand in. That costs
one redundant line per band boundary.

The one genuinely shared thing is the decoded-texture cache, which is **not**
thread-safe (`lookup` mutates its map and per-entry frame stamps). The
coordinator resolves every polygon's texture to a plain pointer before any
worker starts, and workers only read that array back.

## Cost

Each band is a full `Renderer3D` instance: 901 KB, dominated by the edge array
and the output buffer. Three bands cost ~1.8 MB extra. Every band repeats the
edge setup for the whole polygon list (the cursors are walked per line and so
cannot be shared), which is why banding is disabled below 24 polygons.

## Verification

* 16/16 commercial ROMs byte-identical over 400 boot frames, 1 band vs 4.
* All three gameplay replay scenes byte-identical over 900 frames, 1 vs 4.
* ThreadSanitizer clean on all three scenes (no data races).
* `ctest` green; aarch64 cross-build clean.

Frame-exactness against the single-threaded path is the gate that matters here
and it is cheap to re-run, so keep using it (`--dump-frames` + `cmp`) rather
than relying on the melonDS trace diff, which no longer tracks this path.
