#!/bin/bash
# Refresh the portable aarch64 profile inside the old-sysroot container.
#
#   DS_ROMS=<rom dir> DS_BIOS=<bios dir> [DS_DSI=<dsi-binary dir>] \
#     tools/pgo_refresh_container.sh [extra pgo_refresh.sh args]
#
# The portable tarball is built against an old glibc so it runs on spruceOS
# and the other handhelds (see toolchains/rg35xxsp): an old sysroot means an
# old compiler, and a .gcda is tied to the compiler's version, so that build
# needs its own profile. This runs tools/pgo_refresh.sh --native --gcc 10
# inside that image, which writes pgo/aarch64-gcc<version>/ -- the directory
# CMakeLists prefers whenever a build uses that compiler.
#
# On an x86 desktop the container is aarch64 under binfmt, so the training is
# emulated and slow; on an arm64 runner it is native. Either way the build
# runs as the invoking user, so nothing lands root-owned in pgo/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${DS_PGO_IMAGE:-dsperate-u20:arm64}
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
DS_DSI=${DS_DSI:-$HERE/../dsi-binary}
# The image has no git, so the commit the profile is made at comes in by hand;
# without it the MANIFEST would record nothing and --check could never work.
# focal ships SDL 2.0.10, which is too old to compile our frontend (no
# SDL_SYSWM_KMSDRM, no windowID on touch events). toolchains/rg35xxsp/prefix
# holds the SDL 2.30.9 build_sdl.sh makes, which is also the version the
# devices actually run; without it the training succeeds and the verify build
# fails on the SDL frontend alone. It has to be mounted at /work/prefix: its
# CMake/pkg-config files record the absolute path it was built at, which is
# where toolchains/rg35xxsp/build-sdl.sh puts it inside this same image.
SDL_PREFIX=${DS_SDL_PREFIX:-$HERE/../toolchains/rg35xxsp/prefix}
[ -d "$SDL_PREFIX/include/SDL2" ] || {
  echo "no SDL2 at $SDL_PREFIX (run toolchains/rg35xxsp/build-sdl.sh, or set DS_SDL_PREFIX)" >&2; exit 1; }
COMMIT=$(git -C "$HERE" rev-parse HEAD)
[ -z "$(git -C "$HERE" status --porcelain)" ] || {
  echo "working tree is dirty -- commit first, or the profile records a commit it was not built from" >&2; exit 1; }
exec docker run --rm --platform linux/arm64 \
  --user "$(id -u):$(id -g)" \
  -v "$HERE:/src" \
  -v "$DS_ROMS:/roms:ro" \
  -v "$DS_BIOS:/bios:ro" \
  -v "$SDL_PREFIX:/work/prefix:ro" \
  ${DS_DSI:+-v "$DS_DSI:/dsi:ro"} \
  -e DS_ROMS=/roms -e DS_BIOS=/bios -e DS_DSI=/dsi \
  -e DS_PGO_COMMIT="$COMMIT" \
  -e HOME=/tmp \
  "$IMAGE" \
  bash -euc '
    # -pthread is explicit: glibc 2.31 does not pull it in for std::thread.
    exec /src/tools/pgo_refresh.sh --native --gcc 10 /src/build/pgo-gen-focal \
      -DCMAKE_PREFIX_PATH=/work/prefix \
      -DCMAKE_CXX_FLAGS=-pthread \
      "-DCMAKE_EXE_LINKER_FLAGS=-pthread -L/work/prefix/lib -static-libstdc++ -static-libgcc" '"$*"'
  '
