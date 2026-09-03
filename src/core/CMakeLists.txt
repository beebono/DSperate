# SPDX-License-Identifier: GPL-3.0-or-later
add_library(dsperate_core STATIC
  nds.cpp
  profile.cpp
  mem/page_table.cpp
  mem/bus.cpp
  mem/timing.cpp
  dma/dma.cpp
  cart/cart.cpp
  cart/zip.cpp
  cart/miniz/miniz_tinfl.c
  cheat/ar_engine.cpp
  cheat/database.cpp
  sched/scheduler.cpp
  cpu/cpu.cpp
  cpu/cp15.cpp
  cpu/idle_loop.cpp
  cpu/interp/interp.cpp
  cpu/interp/interp_arm.cpp
  cpu/interp/interp_thumb.cpp
  gpu/gpu.cpp
  gpu/engine2d.cpp
  gpu/vram_map.cpp
  gpu/gpu3d.cpp
  gpu/render3d.cpp
  gpu/texcache.cpp
  gpu/kernels_ref.cpp
  spu/spu.cpp
  input/input_log.cpp
  io/io.cpp
)
target_include_directories(dsperate_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The vendored miniz decompression core (cart/miniz/README.md). Only the
# inflate half is compiled: the compressor and miniz's own ZIP reader are not
# vendored, and the archive parsing in cart/zip.cpp is ours.
set_source_files_properties(cart/miniz/miniz_tinfl.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_DEFLATE_APIS;MINIZ_NO_ARCHIVE_APIS;MINIZ_NO_STDIO;MINIZ_NO_TIME"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/cart/miniz")

if(DSPERATE_JIT)
  # The runtime is host-agnostic; the backend directory is the host's.
  target_sources(dsperate_core PRIVATE cpu/jit/runtime.cpp)
  if(DSPERATE_HOST_AARCH64)
    target_sources(dsperate_core PRIVATE cpu/jit/a64/stubs.cpp cpu/jit/a64/translate.cpp)
  elseif(DSPERATE_HOST_ARM32)
    target_sources(dsperate_core PRIVATE cpu/jit/a32/stubs.cpp cpu/jit/a32/translate.cpp)
    target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT_A32=1)
  endif()
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT=1)
endif()
if(DSPERATE_NEON)
  target_sources(dsperate_core PRIVATE gpu/kernels_neon.cpp)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_NEON=1)
  # The kernels reach A64-only intrinsics through gpu/neon_compat.h; on an
  # ARMv7 host that header switches to bodies the A32 encoding can express.
  # DSPERATE_A32_SUBSET forces the same bodies on an AArch64 host.  That is
  # not a 32-bit build -- the JIT and everything else are held fixed -- but it
  # is how the subset's exactness is gated (tools/scene_hashes.sh) and how its
  # instruction mix is measured, neither of which needs an ARMv7 machine.
  if(DSPERATE_HOST_ARM32 OR DSPERATE_A32_SUBSET)
    target_compile_definitions(dsperate_core PUBLIC DS_A32_SUBSET=1)
    message(STATUS "DSperate: NEON kernels restricted to the ARMv7-expressible subset")
  endif()
endif()
