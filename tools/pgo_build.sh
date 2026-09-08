#!/bin/bash
# Optimised aarch64 cross build from the committed PGO profile (pgo/aarch64/),
# exactly what CI does: no ROMs needed. To refresh the profile itself, see
# tools/pgo_refresh.sh.
#
#   tools/pgo_build.sh [build-dir] [extra cmake args]
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build/pgo}; shift || true
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/aarch64-linux-gnu.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_TESTS=OFF -DDSPERATE_PGO=use "$@" > /dev/null
ninja -C "$BUILD" > /dev/null
echo "done: $BUILD/src/frontend/headless/dsperate-headless $BUILD/src/frontend/sdl/dsperate"
