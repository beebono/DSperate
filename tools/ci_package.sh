#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CI's Package step: tools/ci_package.sh PRESET BUILD_DIR ARCH STRIP
#
# Two tarballs from the build CI has just tested in BUILD_DIR:
#   dsperate-<ref>-linux-<ARCH>.tar.gz         dynamically linked, stripped
#   dsperate-<ref>-linux-<ARCH>-static.tar.gz  libstdc++/libgcc inside, debug symbols kept
# The second is a rebuild (RelWithDebInfo, static runtime), so it is a
# different binary from the one the job tested: its tests are built and run
# again before it is packed. The configure keeps every cached setting of the
# first build (PGO among them) and changes only the build type and link flags.
set -euo pipefail
preset=$1; b=$2; arch=$3; strip=$4
d=dsperate-${GITHUB_REF_NAME:-dev}-linux-$arch
bins="$b/src/frontend/sdl/dsperate $b/src/frontend/headless/dsperate-headless"
rm -rf dist/$d dist/$d-static; mkdir -p dist/$d dist/$d-static
cp $bins dist/$d/
$strip dist/$d/dsperate dist/$d/dsperate-headless
cp LICENSE README.md dist/$d/
tar -C dist -czf $d.tar.gz $d
cmake --preset $preset -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build --preset $preset
ctest --test-dir $b --output-on-failure
cp $bins LICENSE README.md dist/$d-static/
tar -C dist -czf $d-static.tar.gz $d-static
ls -l $d.tar.gz $d-static.tar.gz
