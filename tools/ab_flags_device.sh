#!/bin/sh
# Build-flag A/B on the RG DS. Runs every headless variant in /storage/dsperate/ab
# on each scene, alternating in BOTH orders (forward set, then reversed) after a
# warm-up, and prints median / p99 / max per (variant, scene, pass).
#
#   sh /storage/dsperate/ab/ab_flags_device.sh [frames] [rounds]
#
# Frames default 300, rounds default 2 (each round = one forward + one reverse
# pass). Per ab-run-order-bias: believe a win only if it shows in both orders.
set -u
AB=/storage/dsperate/ab
ROMS=/storage/roms/nds
BIOS=/storage/roms/bios
FRAMES=${1:-300}
ROUNDS=${2:-2}
VARIANTS="${VARIANTS:-base noharden mtune mcpu}"
COMMON="--direct --quantum 0 --bios9 $BIOS/bios9.bin --bios7 $BIOS/bios7.bin --firmware $BIOS/firmware.bin"
cd "$AB" || exit 1
md5sum dsperate-headless.* 
run() { # variant scene
  case $2 in
    etody) args="--save $AB/scenes/etody.sav --replay $AB/scenes/etody.dsin --frames $FRAMES";        rom="$ROMS/Etrian Odyssey.nds";;
    dbori) args="--replay $AB/scenes/dbori.dsin --frames $FRAMES";                                    rom="$ROMS/Dragon Ball - Origins.nds";;
    sm64)  args="--save $AB/scenes/sm64.sav --replay $AB/scenes/sm64.dsin --frames $FRAMES";          rom="$ROMS/Super Mario 64 DS.nds";;
    gsdd)  args="--load-state $AB/scenes/gsdd-phase2.dss --frames $FRAMES --stats-from 30";           rom="$ROMS/Golden Sun - Dark Dawn.nds";;
    st)    args="--load-state /storage/roms/savestates/nds/BKIE.0.dss --frames $FRAMES --stats-from 30"; rom="$ROMS/Legend of Zelda, The - Spirit Tracks.nds";;
    mlbis) args="--save $AB/scenes/mlbis.sav --replay $AB/scenes/mlbis.dsin --frames $FRAMES";        rom="$ROMS/Mario & Luigi - Bowser's Inside Story.nds";;
    meteos) args="--save $AB/scenes/meteos.sav --replay $AB/scenes/meteos.dsin --frames $FRAMES";     rom="$ROMS/Meteos.nds";;
  esac
  out=$(./dsperate-headless.$1 $COMMON $args "$rom" 2>&1)
  line=$(echo "$out" | grep '^frame ms: median'); over=$(echo "$out" | grep '^frame budget:' | sed 's/frame budget: \([0-9]*\) of.*/\1/')
  med=$(echo "$line" | sed 's/.*median \([0-9.]*\).*/\1/'); p99=$(echo "$line" | sed 's/.*p99 \([0-9.]*\).*/\1/'); max=$(echo "$line" | sed 's/.*max \([0-9.]*\).*/\1/')
  printf '%-9s %-6s %-4s median %7s p99 %7s max %7s over %s\n' "$1" "$2" "$3" "$med" "$p99" "$max" "$over"
}
echo "warm-up"; run base dbori warm >/dev/null
for scene in ${SCENES:-dbori etody sm64 gsdd st}; do
  r=1
  while [ $r -le $ROUNDS ]; do
    for v in $VARIANTS; do run $v $scene fwd$r; done
    rev=""; for v in $VARIANTS; do rev="$v $rev"; done
    for v in $rev; do run $v $scene rev$r; done
    r=$((r+1))
  done
done
