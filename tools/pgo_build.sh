#!/bin/bash
# Profile-guided aarch64 build: instrument, train under qemu on the recorded
# scenes and the two save-state scenes, rebuild with the profile.
#
#   DS_ROMS=<rom dir> DS_BIOS=<bios dir> tools/pgo_build.sh [build-dir] [extra cmake args]
#
# Both steps use the same build directory: GCC writes each object's .gcda next
# to the object and reads it back from there. ROMs: the five scenes/*.dsin
# titles, Golden Sun - Dark Dawn (scenes/gsdd-phase2.dss) and Spirit Tracks
# (scenes/st-intro.dss). The training runs need the real BIOS and firmware:
# without them the JIT translates one-instruction blocks and the profile
# describes a different program. ~15 minutes on a desktop.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build/pgo}; shift || true
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
Q="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
CONF=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/aarch64-linux-gnu.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_TESTS=OFF "$@")

echo "== instrumented build"
cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=generate > /dev/null
find "$BUILD" -name '*.gcda' -delete
ninja -C "$BUILD" dsperate-headless > /dev/null
G="$BUILD/src/frontend/headless/dsperate-headless"
C=(--direct --quantum 0 --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin")
train() { echo "  train: $1"; shift; $Q "$G" "${C[@]}" "$@" > /dev/null 2>&1 || echo "  (run failed: $*)"; }
echo "== training"
train mlbis  --save "$HERE/scenes/mlbis.sav"  --replay "$HERE/scenes/mlbis.dsin"  --frames 600 "$DS_ROMS/Mario & Luigi - Bowser's Inside Story.nds" &
train sm64   --save "$HERE/scenes/sm64.sav"   --replay "$HERE/scenes/sm64.dsin"   --frames 600 "$DS_ROMS/Super Mario 64 DS.nds" &
train etody  --save "$HERE/scenes/etody.sav"  --replay "$HERE/scenes/etody.dsin"  --frames 600 "$DS_ROMS/Etrian Odyssey.nds" &
train dbori  --replay "$HERE/scenes/dbori.dsin" --frames 600 "$DS_ROMS/Dragon Ball - Origins.nds" &
train meteos --save "$HERE/scenes/meteos.sav" --replay "$HERE/scenes/meteos.dsin" --frames 600 "$DS_ROMS/Meteos.nds" &
train gsdd   --load-state "$HERE/scenes/gsdd-phase2.dss" --frames 400 "$DS_ROMS/Golden Sun - Dark Dawn.nds" &
train st     --load-state "$HERE/scenes/st-intro.dss" --frames 600 "$DS_ROMS/Legend of Zelda, The - Spirit Tracks.nds" &
wait
echo "  profiles: $(find "$BUILD" -name '*.gcda' | wc -l)"

echo "== optimised build"
cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=use > /dev/null
ninja -C "$BUILD" > /dev/null
echo "done: $BUILD/src/frontend/headless/dsperate-headless $BUILD/src/frontend/sdl/dsperate"
