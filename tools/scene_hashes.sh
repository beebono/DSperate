#!/bin/bash
# Per-frame SHA-1 of both framebuffers for a recorded scene, without touching
# the disk: the harness's --dump-frames stream goes through a FIFO into a hasher.
# Two runs of the same scene through different builds (or the same build with
# DS_2D_LAZY=0 / DS_2D_THREAD=0 / --interp) must produce identical files;
# `diff` then names the first frame that differs.
#
#   tools/scene_hashes.sh <dsperate-headless> <scene> <frames> <out.txt> [extra args]
#
# Set DS_ROMS to the directory holding the ROMs and DS_BIOS to the one holding
# bios7.bin, bios9.bin and firmware.bin. Scene names are those in scenes/; the
# ROM file for each is looked up below. An aarch64 build runs the same way
# through a wrapper script that execs qemu-aarch64 with the binary.
set -u
BIN=$1; SCENE=$2; FRAMES=$3; OUT=$4; shift 4
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS to the ROM directory}"
: "${DS_BIOS:?set DS_BIOS to the BIOS/firmware directory}"
case $SCENE in
  mlbis)  ROM="$DS_ROMS/Mario & Luigi - Bowser's Inside Story.nds";;
  meteos) ROM="$DS_ROMS/Meteos.nds";;
  sm64)   ROM="$DS_ROMS/Super Mario 64 DS.nds";;
  etody)  ROM="$DS_ROMS/Etrian Odyssey.nds";;
  dbori)  ROM="$DS_ROMS/Dragon Ball - Origins.nds";;
  *) echo "unknown scene $SCENE" >&2; exit 2;;
esac
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")
FIFO=$(mktemp -u "$OUT.fifo.XXXX"); mkfifo "$FIFO"
python3 - "$FIFO" "$OUT" <<'PY' &
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
       "${SAVE[@]}" --replay "$HERE/scenes/$SCENE.dsin" --frames "$FRAMES" --dump-frames "$FIFO" "$@" "$ROM" 2> "$OUT.log"
wait; rm -f "$FIFO"
