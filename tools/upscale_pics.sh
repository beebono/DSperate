#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Pictures of the upscaled 3D for every scene of tools/scenes.sh, on the host:
#
#   tools/upscale_pics.sh dump OUT [SCENES]    # DSperate at 2x and 4x, and the GPU frame dumps for the reference
#
# For each scene and each of its picture frames F (scenes.sh column 4):
#   OUT/NAME.frames   the frames pictured: those the GPU dumps landed on (run-relative)
#   OUT/NAME.sN.scr   both screens at N x as the GPU present composes them (--dump-screens-scaled), one record per F
#   OUT/NAME.sN.3d    the raster's 3D layer at N x (--dump-3d-fine), one record per F
#   OUT/NAME.fF.gpu   frame F's polygon list (--dump-gpu-frame, + .vram), for the DeSmuME reference renderer
#                     (dsperate-research/tools/desmume/ref3d)
# Records are dsperate-research/tools/desmume/layer_png.py's. HEADLESS (default
# build/host), DS_ROMS / DS_BIOS (default ../resources/ds/...), JOBS (default nproc),
# SCALES (default "2 4"), AA=1 to draw with video.aa (MSAA on the GPU raster; default off,
# as the device baseline runs).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
source "$HERE/tools/scenes.sh"
G=${HEADLESS:-$HERE/build/host/src/frontend/headless/dsperate-headless}
ROMS=${DS_ROMS:-$HERE/../resources/ds/games}
BIOS=${DS_BIOS:-$HERE/../resources/ds/bios}
JOBS=${JOBS:-$(nproc)}
q() { printf '%q ' "$@"; }

case ${1:-} in
  dump)
    out=${2:?OUT}; mkdir -p "$out"
    want=${3:-}
    jobs=$(mktemp); pic_jobs=()
    for s in "${UPSCALE_SCENES[@]}"; do
      IFS='|' read -r name frames from pics args rom start <<< "$s"
      start=${start:-0}
      [ -n "$want" ] && [[ ",$want," != *",$name,"* ]] && continue
      [ -f "$ROMS/$rom" ] || { echo "skip $name: no ROM" >&2; continue; }
      [ -z "$(scene_missing "$name")" ] || { echo "skip $name: scene files missing" >&2; continue; }
      mapfile -t A < <(scene_args "$name")
      base=$(q "$G" --direct --bios9 "$BIOS/bios9.bin" --bios7 "$BIOS/bios7.bin" --firmware "$BIOS/firmware.bin")
      for f in ${pics//,/ }; do
        [ -n "${ONLY_MISSING:-}" ] && [ -f "$out/$name.f$f.gpu" ] && continue
        echo "$base --frames $((f + 60)) $(q --dump-gpu-frame "$((start + f))" "$out/$name.f$f.gpu") $(q "${A[@]}") $(q "$ROMS/$rom") > $(q "$out/$name.f$f.log") 2>&1" >> "$jobs"
      done
      pic_jobs+=("$name|$start|$rom")
    done
    echo "$(wc -l < "$jobs") runs, $JOBS at a time"
    tr '\n' '\0' < "$jobs" | xargs -0 -P "$JOBS" -I{} bash -c {}
    : > "$jobs"
    # The pictures at the frames the dumps landed on (the first frame at or after the one asked for that drew
    # new 3D): NAME.frames lists them, in run-relative numbering, one record each in the .scr/.3d files.
    for e in "${pic_jobs[@]}"; do
      IFS='|' read -r name start rom <<< "$e"
      fr=$(for l in "$out/$name".f*.log; do sed -nE 's/^gpu dump: frame ([0-9]+) .*/\1/p' "$l"; done | sort -n | uniq | awk -v s="$start" '{print $1 - s}' | paste -sd,)
      [ -n "$fr" ] || { echo "skip $name: no frame dumped" >&2; continue; }
      echo "$fr" > "$out/$name.frames"
      mapfile -t A < <(scene_args "$name")
      base=$(q "$G" --direct --bios9 "$BIOS/bios9.bin" --bios7 "$BIOS/bios7.bin" --firmware "$BIOS/firmware.bin")
      for n in ${SCALES-2 4}; do
        echo "$base --frames $((${fr##*,} + 1)) --gpu3d --gpu3d-scale $n $([ -n "${AA:-}" ] || echo --no-aa) --dump-at $fr $(q --dump-screens-scaled "$out/$name.s$n.scr" --dump-3d-fine "$out/$name.s$n.3d" --dump-3d "$out/$name.s$n.1x.3d") $(q "${A[@]}") $(q "$ROMS/$rom") > $(q "$out/$name.s$n.log") 2>&1" >> "$jobs"
      done
    done
    tr '\n' '\0' < "$jobs" | xargs -0 -P "$JOBS" -I{} bash -c {}
    rm -f "$jobs"
    ls "$out" | grep -c '\.gpu$' | xargs echo "gpu dumps:"
    ;;
  *) sed -n 3,15p "$0"; exit 2 ;;
esac
