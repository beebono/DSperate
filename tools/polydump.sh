#!/bin/sh
# Builds the Vulkan raster's fragment stages that write polygon numbers for colours (-DDS_POLY_DEBUG=1) into DIR,
# for DS_R3D_POLYDUMP=1 (src/core/gpu/render3d.cpp poly_dump):
#   tools/polydump.sh DIR
#   DS_R3D_POLYDUMP=1 dsperate-headless ROM --no-aa --dump-3d cpu.3d ...
#   DS_R3D_POLYDUMP=1 DS_VK_SPV_DIR=DIR DS_GPU_NOLAG=1 dsperate-headless ROM --gpu3d --no-aa --dump-3d gpu.3d ...
#   tools/polydump_cmp.py cpu.id gpu.id cpu.3d gpu.3d     (the .id runs with DS_R3D_POLYDUMP=1, the .3d runs without)
# DS_R3D_POLYDUMP=2 with stages built -DDS_POLY_DEBUG=2 (POLY_DEBUG=2 tools/polydump.sh DIR) dumps texture coordinates;
# DS_R3D_POLYLIST=FILE writes each frame's polygons as text.
set -e
out=${1:?usage: polydump.sh DIR}
src=$(cd "$(dirname "$0")/../src/core/gpu/vk/shaders" && pwd)
glslang=${GLSLANG:-$src/../../../../../../toolchains/glslang/bin/glslangValidator}
mkdir -p "$out"
b() { n=$1; shift; "$glslang" -V --target-env vulkan1.1 -I"$src/.." -DDS_POLY_DEBUG=${POLY_DEBUG:-1} "$@" -o "$out/$n.spv" "$src/lean.frag" >/dev/null; }
b lean_frag
b lean_frag_early -DDS_EARLY=1
b lean_frag_opq -DDS_ALPHA_SEL=1
b lean_frag_trans -DDS_ALPHA_SEL=2
b lean_frag_fetch -DDS_FETCH=1 -DDS_ALPHA_SEL=2
b lean_frag_fetch_ds -DDS_FETCH=2 -DDS_ALPHA_SEL=2
