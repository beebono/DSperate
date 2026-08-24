# The recorded benchmark scenes

Five input replays (`--replay`, one 8-byte record per frame) recorded on the
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

| scene | polygon lines / 600 frames | | |
|---|---|---|---|
| | with save | without | |
| `mlbis` | 1,488,184 | 51,713 | replays the file-creation screen, not the recorded battle |
| `meteos` | 208,405 | 0 | draws no 3D at all |
| `sm64` | 317,041 | 316,978 | unaffected; pass it anyway |
| `etody` | 1,520,884 | — | the save *is* the dungeon party |
| `dbori` | ~0 | ~0 | no save, no 3D |

`--save` loads read-only and is never written back, so a replay cannot mutate
its own input.

## What each scene is for

Ranked by how much of the frame the 3D rasteriser takes (host counters,
600 frames, `DS_PROFILE=1`):

| | mean span | depth survival | constant colour | `3d spans` |
|---|---|---|---|---|
| `etody` | 27 px | 71 % | 11 % | 40.2 % |
| `sm64` | 186 px | 99 % | 99.96 % | 28.2 % |
| `mlbis` | 14 px | 59 % | 74 % | 23.6 % |
| `meteos` | 64 px | 75 % | 98 % | 12.4 % |
| `dbori` | — | — | — | 0.2 % |

They are not interchangeable. `sm64` is **pixel-bound** — long spans, almost
no occlusion, flat colour everywhere — and `resolve_span_vec` dominates it.
`etody` and `mlbis` are **setup-bound** — short spans, real overdraw,
interpolated colour — and on both of them `render_polygon_line` is the hottest
function in the process (11.3 % and 9.5 %), ahead of every pixel kernel.

A 3D change ranked on `sm64` alone will be ranked wrong. Use at least one
short-span scene with it.

Timing still belongs on the device (`bench3.sh`); these files make the
*counter* census reproducible on any host, since the counters measure the
workload rather than the machine.
