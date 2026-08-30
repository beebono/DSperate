# The recorded benchmark scenes

Input replays (`--replay`, one 16-byte record per frame; the older 8-byte
logs still play) recorded on the
Anbernic RG DS, with the battery saves they were recorded against. The
emulator is deterministic given its inputs, so a replay reproduces the session
frame for frame — as long as the ROM, the BIOS and **the save** are the same
as when it was recorded.

    dsperate --direct --quantum 0 --bios9 bios9.bin --bios7 bios7.bin \
             --firmware firmware.bin --save scenes/mlbis.sav \
             --replay scenes/mlbis.dsin --frames 600 "Mario & Luigi ….nds"

## Pass the save

The CLI does not pick up `<rom>.sav` automatically, by design — a stray save
next to a ROM must not silently move a frame baseline. So the save has to be
given explicitly, and **a scene replayed without it is a different workload,
not a slightly different one**:

| scene | polygon lines, 1800 frames | | |
|---|---|---|---|
| | with save | without | |
| `mlbis` | **8,173,718** | 4,781,012 | boots into file creation, and every later input lands somewhere else |
| `meteos` | **1,993,443** | 1,414,855 | diverges into different modes |
| `sm64` | 5,391,953 | 5,391,993 | unaffected; pass it anyway |
| `etody` | 5,237,160 | — | the save *is* the dungeon party; there is no scene without it |
| `dbori` | 5,369,005 | — | no save exists for it |

The divergence is a *different sequence of scenes*, not a uniformly lighter
one, and it is worst early: over the first 600 frames Mario & Luigi draws
51,713 polygon lines without its save against 1,488,184 with it, and Meteos
draws no 3D at all against 208,405.

`--save` loads read-only and is never written back, so a replay cannot mutate
its own input.

## Run the whole replay

Every `.dsin` here is **1800 frames**. A 600-frame run covers the first third
and nothing else, which for most of these scenes is boot, logos and a title
screen — measure that and you measure the wrong program. Dragon Ball's 3D
cutscene does not begin until after frame 600; at 600 frames the scene looks
3D-free (0.2 % of the frame), and at 1800 it is 5.37 M polygon lines.

## What each scene is for

Host counters, `DS_PROFILE=1`, full 1800 frames:

| | polygon lines | mean span | depth survival | constant colour | `3d spans` | `3d final pass` |
|---|---|---|---|---|---|---|
| `etody` | 5.24 M | 35 px | 78 % | 18 % | **49.4 %** | 0.8 % |
| `sm64` | 5.39 M | 32 px | 94 % | 54 % | 33.9 % | 0.5 % |
| `mlbis` | **8.17 M** | **13 px** | 75 % | 67 % | 26.8 % | 0.9 % |
| `dbori` | 5.37 M | 17 px | **61 %** | 26 % | 19.0 % | **3.5 %** |
| `meteos` | 1.99 M | 28 px | 93 % | 99 % | 13.7 % | 0.6 % |

All five draw real 3D and all five are **short-span**: 13 to 35 pixels a span,
against the 256-pixel buffers the stages are sized for. On the device,
`render_polygon_line` — per-scanline setup, not pixel work — is the hottest
function in the process on every one of the four 3D-heavy scenes: 11.3 %
(etody), 9.5 % (mlbis), 8.9 % (sm64), 6.6 % (dbori).

What distinguishes them is which secondary cost they expose:

- `etody` has the most span pixels, so the pixel stages weigh heaviest.
- `mlbis` has the shortest spans and the most polygon lines — the worst case
  for anything paid per span rather than per pixel.
- `dbori` is the overdraw scene (39 % of span pixels fail the depth test) and
  the only one where the unfused `final_pass` costs anything (3.5 %). Its
  dual-screen tricks also make it the only scene where the geometry command
  path is hot: `Gpu3D::read` / `fifo_read` / `gxfifo_write` / `submit_polygon`
  together are ~12 %.
- `sm64` has the most nearly-flat content (54 % constant colour), closest to
  the 83 % `docs/techniques/02` reports for DraStic's SM64DS.
- `meteos` is almost entirely flat-coloured and the lightest of the five.

Timing still belongs on the device (`bench3.sh`); these files make the
*counter* census reproducible on any host, since the counters measure the
workload rather than the machine.
