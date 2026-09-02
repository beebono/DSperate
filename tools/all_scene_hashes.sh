#!/bin/bash
# Hash every recorded scene through one binary into a directory, so two
# directories can be diffed to find the first frame any change moved.
#
#   tools/all_scene_hashes.sh <dsperate-cli-or-wrapper> <out-dir> [frames] [extra CLI args]
#
# Runs the five replays (and the Golden Sun phase-2 save state when the ROM is
# present) in parallel. DS_ROMS / DS_BIOS as for scene_hashes.sh.
set -u
BIN=$1; OUT=$2; FRAMES=${3:-1800}; shift 3 2>/dev/null || shift $#
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
mkdir -p "$OUT"
for s in mlbis meteos sm64 etody dbori; do
  "$HERE/tools/scene_hashes.sh" "$BIN" "$s" "$FRAMES" "$OUT/$s.txt" "$@" &
done
GS="$DS_ROMS/Golden Sun - Dark Dawn.nds"
if [ -f "$GS" ]; then
  ( FIFO=$(mktemp -u "$OUT/gsdd.fifo.XXXX"); mkfifo "$FIFO"
    python3 - "$FIFO" "$OUT/gsdd.txt" <<'PY' &
import sys, hashlib
f = open(sys.argv[1], 'rb'); out = open(sys.argv[2], 'w'); n = 0
FR = 256 * 192 * 4 * 2
while True:
    d = f.read(FR)
    if len(d) < FR: break
    out.write(f"{n} {hashlib.sha1(d).hexdigest()}\n"); n += 1
out.close()
PY
    "$BIN" --direct --quantum 0 --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
      --load-state "$HERE/scenes/gsdd-phase2.dss" --frames 300 --dump-frames "$FIFO" "$@" "$GS" 2> "$OUT/gsdd.log"
    wait; rm -f "$FIFO" ) &
fi
wait
wc -l "$OUT"/*.txt
