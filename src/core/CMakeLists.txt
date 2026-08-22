# SPDX-License-Identifier: GPL-3.0-or-later
add_library(dsperate_core STATIC
  nds.cpp
  profile.cpp
  mem/page_table.cpp
  mem/bus.cpp
  mem/timing.cpp
  dma/dma.cpp
  cart/cart.cpp
  sched/scheduler.cpp
  cpu/cpu.cpp
  cpu/cp15.cpp
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

if(DSPERATE_JIT)
  target_sources(dsperate_core PRIVATE cpu/jit/runtime.cpp cpu/jit/translate.cpp)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT=1)
endif()
if(DSPERATE_NEON)
  target_sources(dsperate_core PRIVATE gpu/kernels_neon.cpp)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_NEON=1)
endif()
