#!/bin/sh
# Compile the frontend's Vulkan present shader to SPIR-V and check the blob in.
#
# The .spv files next to the .comp/.vert/.frag sources are generated but tracked, so a
# build needs no shader compiler -- gpu_present.cpp .incbin's them, the way
# io/dsi_font.cpp does its font tables. Run this after editing any shader and
# commit the .spv beside it.
#
#   tools/gen_shaders.sh
#
# The compiler is, in order of preference:
#   $GLSLANG                          an explicit path (e.g. a prebuilt glslangValidator)
#   glslangValidator / glslc          whatever is on PATH
# Blobs are validated with spirv-val when it is available, because a shader
# that compiles is not necessarily one the driver will accept.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)

if [ -n "$GLSLANG" ]; then GV="$GLSLANG"
elif command -v glslangValidator >/dev/null 2>&1; then GV=glslangValidator
elif command -v glslc >/dev/null 2>&1; then GV=glslc
else
  echo "gen_shaders: no GLSL compiler found." >&2
  echo "  fetch a prebuilt one and point GLSLANG at its bin/glslangValidator:" >&2
  echo "              curl -sSL -o g.zip https://github.com/KhronosGroup/glslang/releases/download/master-tot/glslang-master-linux-Release.zip &&" >&2
  echo "              unzip -oq g.zip -d glslang && rm g.zip" >&2
  exit 1
fi

# x.comp -> x.spv; x.vert / x.frag -> x_vert.spv / x_frag.spv.
for f in "$DIR"/src/frontend/sdl/shaders/*.comp "$DIR"/src/frontend/sdl/shaders/*.vert "$DIR"/src/frontend/sdl/shaders/*.frag; do
  [ -e "$f" ] || continue
  case "$f" in *.comp) out="${f%.comp}.spv" ;; *) out="${f%.*}_${f##*.}.spv" ;; esac
  case "$GV" in
    *glslc) "$GV" -O --target-env=vulkan1.1 "$f" -o "$out" ;;
    *)      log=$("$GV" -V --target-env vulkan1.1 "$f" -o "$out") || { echo "$log" >&2; echo "gen_shaders: $f FAILED" >&2; exit 1; } ;;
  esac
  if command -v spirv-val >/dev/null 2>&1; then spirv-val "$out"; fi
  printf '%-16s %6d bytes\n' "$(basename "$out")" "$(wc -c < "$out")"
done
# present.frag with the 3D merge (DS_MERGE): its own blob, so the plain present's stays as it is.
f="$DIR/src/frontend/sdl/shaders/present.frag"; out="$DIR/src/frontend/sdl/shaders/present_merge_frag.spv"
case "$GV" in
  *glslc) "$GV" -O --target-env=vulkan1.1 -DDS_MERGE=1 "$f" -o "$out" ;;
  *)      log=$("$GV" -V --target-env vulkan1.1 -DDS_MERGE=1 "$f" -o "$out") || { echo "$log" >&2; echo "gen_shaders: $f (merge) FAILED" >&2; exit 1; } ;;
esac
if command -v spirv-val >/dev/null 2>&1; then spirv-val "$out"; fi
printf '%-16s %6d bytes\n' "$(basename "$out")" "$(wc -c < "$out")"
echo "gen_shaders: done -- commit the .spv files beside their sources"
