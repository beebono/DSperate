#!/bin/bash
# Save-state round trip on a recorded scene: run N frames, save, keep going
# to N+M and hash every frame; then load that state into a fresh process and
# run M frames. The hashes after the load must equal the ones after the save,
# and a state saved straight after the load must be byte-identical to the one
# loaded (a field that was forgotten shows up in one or the other).
#
#   tools/state_roundtrip.sh <dsperate-cli> <scene> <N> <M> [extra CLI args]
#
# DS_ROMS / DS_BIOS as for scene_hashes.sh. Exit status is the verdict.
set -u
BIN=$1; SCENE=$2; N=$3; M=$4; shift 4
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS to the ROM directory}"
: "${DS_BIOS:?set DS_BIOS to the BIOS/firmware directory}"
case "$SCENE" in
  mlbis)  ROM="$DS_ROMS/Mario & Luigi - Bowser's Inside Story.nds";;
  meteos) ROM="$DS_ROMS/Meteos.nds";;
  sm64)   ROM="$DS_ROMS/Super Mario 64 DS.nds";;
  etody)  ROM="$DS_ROMS/Etrian Odyssey.nds";;
  dbori)  ROM="$DS_ROMS/Dragon Ball - Origins.nds";;
  *) echo "unknown scene $SCENE" >&2; exit 2;;
esac
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
COMMON=(--direct --quantum 0 --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" "${SAVE[@]}" --replay "$HERE/scenes/$SCENE.dsin")
hashes() {   # frames from a --dump-frames stream: one sha1 per frame (both screens)
  python3 -c '
import sys, hashlib
n = 256*192*4*2; i = 0
while True:
  d = sys.stdin.buffer.read(n)
  if len(d) < n: break
  print(i, hashlib.sha1(d).hexdigest()); i += 1'
}
# Reference: one run through N+M frames, state after N, hashes of frames N..N+M-1.
DS_FRAME_HASH=1 "$BIN" "${COMMON[@]}" --frames $((N + M)) --save-state-at "$N:$T/state.dss" --dump-frames /dev/stdout "$@" "$ROM" 2> "$T/ref.log" | hashes | tail -n "$M" | cut -d' ' -f2 > "$T/ref.txt"
grep '^\[fh\]' "$T/ref.log" | tail -n "$M" | cut -d' ' -f3- > "$T/ref.fh"
[ -s "$T/state.dss" ] || { echo "no state written"; cat "$T/ref.log"; exit 1; }
# Load it and run M frames; save again at once for the idempotence check.
DS_FRAME_HASH=1 "$BIN" "${COMMON[@]}" --load-state "$T/state.dss" --save-state-at "0:$T/again.dss" --frames "$M" --dump-frames /dev/stdout "$@" "$ROM" 2> "$T/load.log" | hashes | cut -d' ' -f2 > "$T/load.txt"
grep '^\[fh\]' "$T/load.log" | cut -d' ' -f3- > "$T/load.fh"
status=0
if cmp -s "$T/state.dss" "$T/again.dss"; then echo "$SCENE @$N: save-after-load identical ($(stat -c %s "$T/state.dss") bytes)"; else echo "$SCENE @$N: save-after-load DIFFERS"; cmp "$T/state.dss" "$T/again.dss" | head -1; status=1; fi
first_diff() { paste -d' ' "$1" "$2" | awk '$1 != $2 { print NR - 1; exit }'; }   # 0-based frame after the load
if diff -q "$T/ref.fh" "$T/load.fh" > /dev/null; then echo "$SCENE @$N+$M: RAM/register hashes match"; else echo "$SCENE @$N+$M: RAM/register hashes DIFFER from frame $(first_diff "$T/ref.fh" "$T/load.fh") after the load"; status=1; fi
if diff -q "$T/ref.txt" "$T/load.txt" > /dev/null; then echo "$SCENE @$N+$M: frame hashes match ($(wc -l < "$T/load.txt") frames)"; else echo "$SCENE @$N+$M: frame hashes DIFFER from frame $(first_diff "$T/ref.txt" "$T/load.txt") after the load"; status=1; fi
[ $status -ne 0 ] && { echo "--- load log"; tail -5 "$T/load.log"; }
exit $status
