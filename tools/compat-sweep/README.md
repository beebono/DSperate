# Compatibility sweep

Boots every game in a ROM set headless, with and without input, and sorts the
results so the outliers can be looked at by hand. The set is only ever read:
each zipped ROM is unpacked by the runner into a scratch directory under the
output directory for its run and deleted after, no save is written back, and
every output goes to that separate directory (`run_sweep.py` refuses one inside
the set). The headless must not open the zips itself: its zip cache
(`cart/zip_cache.h`) unpacks beside the archive, into `<set>/.dsperate/`, and
the runner stops if that directory ever appears.

```sh
# 0. (a set still being copied in) wait for it to settle, then list it
tools/compat-sweep/wait_settled.sh "$SET" -- tools/compat-sweep/tabulate.py "$SET" "$OUT"
# 1. no input, 90 s: boot, title screen, attract mode
tools/compat-sweep/run_sweep.py --set "$SET" --list "$OUT/titles.tsv" --out "$OUT" --phase idle --bios "$DS_BIOS"
# 2. the standard mash, 90 s
tools/compat-sweep/run_sweep.py --set "$SET" --list "$OUT/titles.tsv" --out "$OUT" --phase mash --bios "$DS_BIOS"
# 3. the JIT check (below)
tools/compat-sweep/run_sweep.py --set "$SET" --list "$OUT/titles.tsv" --out "$OUT" --phase jit --frames 1800 --bios "$DS_BIOS"
# 4. categories, outliers, screenshot sheets
tools/compat-sweep/classify.py "$OUT"
```

Every phase resumes: a game already in `OUT/results-<phase>.jsonl` is skipped,
so a run can be stopped and started again. `--only REGEX` and `--limit N` pick a
subset; `--frames` sets the length; `--extra "..."` passes headless arguments
(`--dsi` for a DSi-mode pass over the DSi-enhanced titles, for example).

## Files

| file | what it does |
|---|---|
| `tabulate.py SET OUT` | reads each zip's central directory: `games.tsv` (one row per dump, No-Intro tags parsed, broken zips flagged), `titles.tsv` (one row per title with the dump to sweep: retail over beta/demo/kiosk, cartridge over Virtual Console, USA > World > Europe > Japan, newest revision) |
| `wait_settled.sh DIR [-- CMD]` | waits until DIR has had no change in file count, bytes or newest mtime for `QUIET_SECS` (900), then runs CMD |
| `run_sweep.py` | the runner: one headless run per game in parallel (`--jobs`, default half the cores, since a job keeps ~2 cores busy with the 2D and 3D workers) |
| `classify.py OUT` | `classify.tsv`, `summary.md` (counts and the outlier list), `sheet-<phase>-<category>.html` |

## Signals

Each run writes `dsperate-headless --sweep-log FILE`: one line per frame with
both screens' hashes (the same FNV-1a as `--hash-frames`), each screen's fill
colour when it is one colour, the 3D polygon count, 2D register/palette/OAM
writes per engine, both DISPCNTs and MASTER_BRIGHTs, the audio peak, CPU faults
(undefined instruction and abort exceptions) and both PCs. Five screenshots per
run (frames 300, 1200, 2400, 3600 and the last) go into one PNG strip.

Categories, judged on the last 30 s: `failed`, `no-boot`, `faulting`,
`frozen` (picture and sound stopped; a still, silent title screen looks the same, so
an idle `frozen` only counts when the mash does not get past it), `static`
(picture stopped, sound plays),
`loop` (a few pictures cycling), `animated-2d`, `animated-3d`. The mash phase is
also set against idle: `progressed`, `same` (the input changed nothing: the
game wants another button or a touch somewhere else) or `regressed`.

The mash (`mash-<frames>.dsin`, written by `tools/mkdsin.py`): nothing for 120
frames, then every 40 frames A, Start and a tap in the bottom screen's centre
column (x 128, y cycling 96, 72, 120, 48, 144: where menus put their buttons),
4 frames each, never two at once (L+R+Start+Select resets many games) and no B
(it backs out of menus).

## The JIT check

The JIT's frame hashes equal the interpreter's only in the mode the golden
check uses: `DS_JIT_STRICT=1` (per-instruction budget checks),
`DS_TIMING=exact` and `DS_IDLE_CUT=0`. The default mode interleaves the CPUs
differently by design: Golden Sun: Dark Dawn shows the same pictures a frame
later from frame 220 on, on the device as under qemu. So `--phase jit` runs
the host interpreter and the AArch64 build under `qemu-aarch64-static`, both
in that mode and both with exact `--frameskip 59`, and compares the screen
hashes; the first differing frame is the lead.

Measured 2026-11-24 (12-core x86 dev box, 3000 frames from boot):

- qemu's JIT is the device's JIT: over 400 frames of Golden Sun the RG DS Plus and qemu gave
  bit-identical hashes, in the default mode as well.
- Cost against the host interpreter (wall / CPU seconds): Mario Kart DS 6.3/7.2
  host, 22/22 qemu; Super Mario 64 DS 3.7/4.5 host, 9.9/10.2 qemu; Golden Sun:
  Dark Dawn 8.9/19 host, 54/85 qemu (it captures, so most of its frames still
  render).
- `--frameskip 59` keeps the guest exact (capture frames are drawn), and under
  qemu most of the saving comes from not running the render threads through
  the emulator: Spirit Tracks' qemu CPU time is 77 s drawing everything and
  18 s with `--frameskip-capture` too (not exact, so not used here).
- Hashing RAM per frame (`DS_FRAME_HASH`) is not a usable gate: the JIT does
  not compute flags nobody reads, so a CPSR the game saves to RAM (an IRQ or
  thread switch) can differ in a dead flag bit (Golden Sun, one byte, frames
  218-222). The screens are the gate, as in `tools/video_golden.sh`.
- `DS_GPU3D_API=null --gpu3d` (the null GPU raster) halves qemu's CPU time on
  capture-heavy games, but the GPU route's list build is not bit-identical
  between x86 and AArch64 (Golden Sun differs at frame 2068), so it cannot be
  compared across the two hosts.

The ARMv7 backend can be checked the same way with
`--jit-headless build/arm32/... --qemu "qemu-arm-static -L /usr/arm-linux-gnueabihf"`.
