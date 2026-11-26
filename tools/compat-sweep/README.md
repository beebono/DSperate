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
| `tabulate.py SET OUT` | reads each zip's central directory: `games.tsv` (one row per dump, No-Intro tags parsed, its kind, broken zips flagged), `titles.tsv` (one row per title with the dump to sweep: retail over beta/demo/kiosk, cartridge over Virtual Console, USA > World > Europe > Japan, newest revision) |
| `romsets.py` | what to run from a zip, shared by the two above: the image of a cartridge dump (`.nds`, `.dsi`, iQue `.ids`), or for DSiWare in the CDN layout (content files, `tmd.<version>`, `titleKey.bin`) the app (content 0) of the newest version's TMD |
| `wait_settled.sh DIR [-- CMD]` | waits until DIR has had no change in file count, bytes or newest mtime for `QUIET_SECS` (900), then runs CMD |
| `run_sweep.py` | the runner: one headless run per game in parallel (`--jobs`, default half the cores, since a job keeps ~2 cores busy with the 2D and 3D workers) |
| `classify.py OUT` | `classify.tsv`, `summary.md` (counts and the outlier list), `sheet-<phase>-<category>.html` |
| `review.py DIR [SET...]` | `DIR/REVIEW.md`: the games worth looking at by eye (broken, regressed, stuck, ignoring input, frozen after input, no progress), each with its idle and mash screenshot strips, over one or more sets' outputs under DIR; with `--menus [--second DIR2]`, `DIR/MENUS.md` instead: a checklist of the games that took input but may not have got past their first menus, each with the mash run (DIR2: a wide-mash pass) that took the most input |
| `probe_touch.py` | for games stuck on a screen: replays the mash with a save state at its end, taps the bottom screen on a grid from it and measures how much of the picture each tap changes (heat map per game); `--verify` flags taps that go back to a screen the run already showed (a Back or No); `--plan` picks the fewest tap points that move the most games on, for a wider mash |

## Signals

Each run writes `dsperate-headless --sweep-log FILE`: one line per frame with
both screens' hashes (the same FNV-1a as `--hash-frames`), each screen's fill
colour when it is one colour, the 3D polygon count, 2D register/palette/OAM
writes per engine, both DISPCNTs and MASTER_BRIGHTs, the audio peak, CPU faults
(undefined instruction and abort exceptions) and both PCs. Five screenshots per
run (frames 300, 1200, 2400, 3600 and the last) go into one PNG strip.

Categories, judged on the last 30 s: `failed` (crash, timeout), `exited` (the
run ended cleanly early: the title quit, as DSiWare does when it goes back to
the DSi Menu), `no-boot`, `faulting`,
`frozen` (picture and sound stopped; a still, silent title screen looks the same, so
an idle `frozen` only counts when the mash does not get past it), `static`
(picture stopped, sound plays),
`loop` (a few pictures cycling), `animated-2d`, `animated-3d`. The mash phase is
also set against idle: `progressed` (the game took the input: at least 3 of
the mash's presses or taps changed the picture within 10 frames, counted only
where the picture was still before the press or the answer is a picture the idle
run never showed), `same` (the input changed nothing: the
game wants another button or a touch somewhere else) or `regressed`.

The mash (`mash-<frames>.dsin`, written by `tools/mkdsin.py`): nothing for 120
frames, then every 40 frames A, Start and a tap in the bottom screen's centre
column (x 128, y cycling 96, 72, 120, 48, 144: where menus put their buttons),
4 frames each, never two at once (L+R+Start+Select resets many games) and no B
(it backs out of menus).

`--mash wide` (`mash-wide-<frames>.dsin`) is for a second pass over the games
the standard mash left stuck: every 72 frames A, Start, a tap cycling through
the 12 points `probe_touch.py --plan` picked over the 206 stuck games of the
2026-11-25 sweep (`WIDE_TAPS` in `run_sweep.py`: the left button of a pair,
keyboard OK corners, flags), D-pad Down (held-sideways menus: Flametail) and a
left-to-right swipe along the bottom of the bottom screen (page turns: the
classic book collections, Peakvox Route Candle). Give it its own `--out`; the
runner writes the schedule to `OUT/mash.json`, which `classify.py` counts
accepted inputs by. For the comparison with idle, copy the games' lines of the
first pass's `results-idle.jsonl` into the new `--out` and link its
`logs/idle` and `shots/idle`.

## DSi cartridges and DSiWare

The runner picks the machine per dump (`--dsi-bios DIR` or `$DS_DSI_BIOS`:
`dsi_bios9.bin`, `dsi_bios7.bin`, `dsi_firmware.bin`): DS cartridges run as a
DS, DSi-enhanced ones included (`--dsi-mode` runs them as a DSi, for a second
pass); DSi cartridges as a DSi; DSiWare as a DSi with `--dsi-hle-launch` (the
app handed over as the DSi launcher does, on a NAND synthesised in memory: no
NAND dump needed). A DSi runs on the exact timing model (both frontends choose
it unless told otherwise). `(Program)` dumps (factory test software: most of
the No-Intro DSi cartridge set) are left out unless `--exclude ''`.

Open (2026-11-24): the strict JIT does not match the interpreter on several
DSi titles under exact timing, reproducibly and with the interpreter
deterministic (the qemu interpreter matches the host): DSiWare 101 Pinball
World from frame 179, 10-Byou Sou, 101 MiniGolf World, 101 Shark Pets; the
DSi cartridges System - Flaw and NOE Movie Player. A DS cartridge in DSi mode
(Super Mario 64 DS) matches, so it is not DSi mode itself but something these
titles use (NAND saves through SD/MMC and NDMA, the cameras: unverified).
`tools/video_golden.sh` has no DSi scene, so this was never gated.

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
