#!/bin/bash
# Optimised cross build from the committed PGO profile, exactly what CI does:
# no ROMs needed. To refresh a profile itself, see tools/pgo_refresh.sh.
#
#   tools/pgo_build.sh [--arm32] [build-dir] [extra cmake args]
#
# --arm32 builds the ARM32 (armv7l) tier from pgo/armv7l with the vendored A30
# toolchain -- which is the only compiler that profile will be accepted by, the
# .gcda format being tied to the compiler's minor version. Worth the trouble:
# on the A30 it is a 6-7 % saving on New Super Mario Bros. from boot (median
# 12.99 -> 12.20 ms, p99 28.8 -> 26.1, both orders) and 3-6 % on the SM64DS
# replay. DS_A30_TOOLCHAIN overrides where that toolchain lives.
set -eu
ARM32=0; if [ "${1:-}" = "--arm32" ]; then ARM32=1; shift; fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ $ARM32 = 1 ]; then
  A30=${DS_A30_TOOLCHAIN:-$HERE/../toolchains/a30}
  TOOLCHAIN="$A30/tc-a30.cmake"
  DEFAULT_BUILD=$HERE/build/pgo-armv7l
  [ -f "$TOOLCHAIN" ] || { echo "no ARM32 toolchain at $A30 (set DS_A30_TOOLCHAIN); see toolchains/a30"; exit 1; }
else
  TOOLCHAIN="$HERE/cmake/aarch64-linux-gnu.cmake"
  DEFAULT_BUILD=$HERE/build/pgo
fi
BUILD=${1:-$DEFAULT_BUILD}; shift || true
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_TESTS=OFF -DDSPERATE_PGO=use "$@" > /dev/null
ninja -C "$BUILD" > /dev/null
echo "done: $BUILD/src/frontend/headless/dsperate-headless $BUILD/src/frontend/sdl/dsperate"
