#!/bin/bash
# Regenerate the committed PGO profile (pgo/<arch>/), or check whether it has
# drifted from the tree it will be applied to.
#
#   DS_ROMS=<rom dir> DS_BIOS=<bios dir> tools/pgo_refresh.sh [--check] [build-dir] [extra cmake args]
#
# --check  compares the profile's MANIFEST against this tree: the compiler and
#          the compile flags (a mismatch voids the whole profile -- CI fails the
#          configure on it), and the source files changed since the profile was
#          made (each edited function silently loses its profile). Exit 1 if
#          the compiler/flags differ, 2 if only sources moved, 0 if current.
# default  an instrumented aarch64 cross build, trained under qemu on the
#          recorded scenes and the two save-state scenes, writes the .gcda
#          files into pgo/aarch64/ and a MANIFEST; then a "use" build from
#          the committed profile in a *different* directory, reporting how many
#          functions failed to find their profile (should be none straight
#          after a refresh). ~15 minutes on a desktop. Commit pgo/ afterwards.
#
# The training needs the real BIOS and firmware: without them the JIT
# translates one-instruction blocks and the profile describes another program.
set -eu
CHECK=0; if [ "${1:-}" = "--check" ]; then CHECK=1; shift; fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build/pgo-gen}; shift || true
ARCH=aarch64
PROFILE="$HERE/pgo/$ARCH"
Q="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
CONF=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/$ARCH-linux-gnu.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_TESTS=OFF "$@")

fingerprint() {   # of a configured build dir
  cat "$1/pgo-fingerprint"
}

if [ $CHECK = 1 ]; then
  [ -f "$PROFILE/MANIFEST" ] || { echo "no profile at $PROFILE"; exit 1; }
  rm -rf "$BUILD"
  cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=OFF > /dev/null
  now=$(fingerprint "$BUILD"); was=$(sed -n 's/^fingerprint //p' "$PROFILE/MANIFEST")
  commit=$(sed -n 's/^commit //p' "$PROFILE/MANIFEST")
  echo "profile: $(sed -n 's/^date //p' "$PROFILE/MANIFEST"), commit $commit, $(sed -n 's/^compiler //p' "$PROFILE/MANIFEST")"
  if [ "$now" != "$was" ]; then echo "DRIFT: compiler or flags differ (profile $was, tree $now) -- the profile is void"; exit 1; fi
  changed=$(git -C "$HERE" diff --name-only "$commit" -- src | wc -l)
  if [ "$changed" -gt 0 ]; then
    echo "source drift: $changed files under src/ changed since the profile ($(git -C "$HERE" diff --shortstat "$commit" -- src | sed 's/^ //'))"
    git -C "$HERE" diff --name-only "$commit" -- src | sed 's/^/  /'
    exit 2
  fi
  echo "current"; exit 0
fi

: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
echo "== instrumented build -> $PROFILE"
mkdir -p "$PROFILE"
find "$PROFILE" -name '*.gcda' -delete
rm -rf "$BUILD" "$BUILD-use"   # scratch directories: a stale cache (flags) would fingerprint the profile wrongly
cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=generate > /dev/null
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
n=$(find "$PROFILE" -name '*.gcda' | wc -l)
echo "  profiles: $n"
[ "$n" -gt 0 ] || { echo "no profile written"; exit 1; }
{
  echo "fingerprint $(fingerprint "$BUILD")"
  echo "compiler $(aarch64-linux-gnu-g++ -dumpfullversion) ($(aarch64-linux-gnu-g++ -dumpmachine))"
  echo "commit $(git -C "$HERE" rev-parse HEAD)"
  echo "date $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "scenes mlbis sm64 etody dbori meteos gsdd st"
} > "$PROFILE/MANIFEST"

echo "== verifying: optimised build from the committed profile (separate build dir)"
VBUILD="$BUILD-use"
cmake -S "$HERE" -B "$VBUILD" "${CONF[@]}" -DDSPERATE_PGO=use -DDSPERATE_PGO_STRICT=ON > /dev/null
ninja -C "$VBUILD" > "$VBUILD/pgo-warnings.log" 2>&1 || { tail -20 "$VBUILD/pgo-warnings.log"; exit 1; }
# -Wmissing-profile is per object (no .gcda at all), -Wcoverage-mismatch per
# function (the profile is there but the function changed). The SDL frontend's
# objects never run in the training, so their missing profiles are expected.
core_missing=$(grep -c "data file not found" <(grep -v "src/frontend/sdl/" "$VBUILD/pgo-warnings.log") || true)
sdl_missing=$(grep "src/frontend/sdl/" "$VBUILD/pgo-warnings.log" | grep -c "data file not found" || true)
mismatch=$(grep -c "control flow of function" "$VBUILD/pgo-warnings.log" || true)   # "source locations ... changed" is benign: the counts still apply
echo "  objects without a profile: core/headless $core_missing (miniz and kernels_ref never run: 3 expected), sdl frontend $sdl_missing (untrained, expected)"
[ "$core_missing" -le 3 ] || grep -v "src/frontend/sdl/" "$VBUILD/pgo-warnings.log" | grep "data file not found" | sed -E 's/.*dir#(.*)\.gcda.*/    \1/'
echo "  functions whose profile no longer matches: $mismatch (should be 0 straight after a refresh)"
echo "done: $PROFILE ($n files); binaries in $VBUILD"
