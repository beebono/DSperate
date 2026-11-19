#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The upscaled-3D scene set: one table shared by the baseline, bisect and
# picture runs, so every phase measures the same thing.
#
#   source tools/scenes.sh                # defines UPSCALE_SCENES and the helpers below
#   tools/scenes.sh list                  # print the table (missing ROMs / scene files marked)
#   tools/scenes.sh args NAME [ROMDIR]    # the scene's arguments, one per line (no frontend flags)
#
# Columns: name | frames | stats-from | picture frames | args | ROM [| start]
# - frames, stats-from and picture frames count from the run's first frame; start is that
#   frame's own number when the scene begins from a save state (what --dump-gpu-frame counts).
# - args are paths relative to the repo's scenes/ written as @/file and accepted by
#   both frontends (--replay, --save, --load-state); headless also needs --direct.
# - stats-from skips boot and loading so the frame report covers the 3D stretch.
# - picture frames are where Phase 3 dumps 3D layers (comma-separated).
# Pokémon White 2 is covered by White 1; Kirby SSU is left out (mostly 2D).
UPSCALE_SCENES=(
  "sm64|1800|300|600,1200,1700|--replay @/sm64.dsin|Super Mario 64 DS.nds"
  "meteos|1800|300|600,1200,1700|--replay @/meteos.dsin|Meteos.nds"
  "dbori|1800|300|600,1200,1700|--replay @/dbori.dsin|Dragon Ball - Origins.nds"
  "etody|1800|300|600,1200,1700|--replay @/etody.dsin|Etrian Odyssey.nds"
  "mlbis|1800|300|600,1200,1700|--save @/mlbis.sav --replay @/mlbis.dsin|Mario & Luigi - Bowser's Inside Story.nds"
  "pkmnwhite|2797|1200|1500,2200,2700|--save @/pkmnwhite.sav --replay @/pkmnwhite.dsin|Pokemon - White Version.nds"
  "ghontour|2400|300|600,1500,2300|--guitar-grip --save @/ghontour.sav --load-state @/ghontour.dss --replay @/ghontour.dsin|Guitar Hero - On Tour.nds|1612"
  "mkds|1800|120|300,900,1700|--save @/mkds.sav --load-state @/mkds.dss|Mario Kart DS.nds|1340"
  "gsdd|2400|900|1200,1800,2350|--|Golden Sun - Dark Dawn.nds"
  "st|2400|900|1200,1800,2350|--|Legend of Zelda, The - Spirit Tracks.nds"
  "nsmb|2400|1200|1300,1800,2350|--|New Super Mario Bros..nds"
  "ph|2400|900|1200,1800,2350|--|Legend of Zelda, The - Phantom Hourglass.nds"
  "solatorobo|3200|1100|1500,2400,3100|--save @/solatorobo.sav --replay @/solatorobo.dsin|Solatorobo - Red the Hunter.nds"
)

UPSCALE_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# scene_field NAME COLUMN -> the column (1-based) of NAME's row
scene_field() {
  local s
  for s in "${UPSCALE_SCENES[@]}"; do
    [ "${s%%|*}" = "$1" ] && { cut -d'|' -f"$2" <<< "$s"; return 0; }
  done
  echo "no scene $1" >&2; return 1
}

# scene_args NAME [SCENEDIR] -> the scene's arguments, one per line, @/ resolved
# against SCENEDIR (default the repo's scenes/; the device copy uses its own).
scene_args() {
  local dir=${2:-$UPSCALE_HERE/scenes} a
  for a in $(scene_field "$1" 5); do
    [ "$a" = -- ] && continue
    printf '%s\n' "${a//@\//$dir/}"
  done
}

# scene_missing NAME [SCENEDIR] -> the scene files that are not there (empty if complete)
scene_missing() {
  local a
  while IFS= read -r a; do
    case $a in --*) ;; *) [ -e "$a" ] || echo "$a" ;; esac
  done < <(scene_args "$@")
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case ${1:-} in
    list)
      roms=${2:-${DS_ROMS:-$UPSCALE_HERE/../resources/ds/games}}
      for s in "${UPSCALE_SCENES[@]}"; do
        IFS='|' read -r name frames from pics args rom <<< "$s"
        note=""
        [ -f "$roms/$rom" ] || note+=" [no ROM]"
        m=$(scene_missing "$name"); [ -n "$m" ] && note+=" [missing: $(basename -a $m | tr '\n' ' ')]"
        printf '%-11s %5s from %-5s pics %-15s %s%s\n' "$name" "$frames" "$from" "$pics" "$rom" "$note"
      done ;;
    args) scene_args "${2:?NAME}" "${3:-}" ;;
    *) sed -n 3,15p "$0"; exit 2 ;;
  esac
fi
