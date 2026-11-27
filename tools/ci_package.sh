#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CI's packaging: tools/ci_package.sh plain|static PRESET BUILD_DIR ARCH STRIP [cmake args..]
#
#   plain   dsperate-<ref>-linux-<ARCH>.tar.gz: dynamically linked and stripped,
#           from the build the job has just tested in BUILD_DIR.
#   static  dsperate-<ref>-linux-<ARCH>-static.tar.gz: libstdc++/libgcc inside,
#           debug symbols kept. Its own build (RelWithDebInfo, static runtime) in
#           its own job, beside the plain one rather than after it; a different
#           binary from the one the plain job tested, so its tests are built and
#           run before it is packed. The cmake args are those the plain job's
#           build was configured with beyond its preset (arm64: its PGO), so the
#           two differ only in build type and link flags.
set -euo pipefail
mode=$1; preset=$2; b=$3; arch=$4; strip=$5; shift 5
d=dsperate-${GITHUB_REF_NAME:-dev}-linux-$arch
bins="$b/src/frontend/sdl/dsperate $b/src/frontend/headless/dsperate-headless"
case $mode in
  plain)
    rm -rf dist/$d; mkdir -p dist/$d
    cp $bins dist/$d/
    $strip dist/$d/dsperate dist/$d/dsperate-headless
    cp LICENSE README.md dist/$d/
    tar -C dist -czf $d.tar.gz $d
    ls -l $d.tar.gz ;;
  static)
    cmake --preset $preset "$@" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
    cmake --build --preset $preset
    ctest --test-dir $b --output-on-failure
    rm -rf dist/$d-static; mkdir -p dist/$d-static
    cp $bins LICENSE README.md dist/$d-static/
    tar -C dist -czf $d-static.tar.gz $d-static
    ls -l $d-static.tar.gz ;;
  *) echo "mode: plain or static" >&2; exit 2 ;;
esac
