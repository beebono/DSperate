# SPDX-License-Identifier: GPL-3.0-or-later
add_library(dsperate_core STATIC
  nds.cpp
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
  spu/spu.cpp
  io/io.cpp
)
target_include_directories(dsperate_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(DSPERATE_JIT)
  target_sources(dsperate_core PRIVATE cpu/jit/jit.cpp)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_JIT=1)
endif()
if(DSPERATE_NEON)
  target_compile_definitions(dsperate_core PUBLIC DSPERATE_NEON=1)
endif()
