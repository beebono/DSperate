# SPDX-License-Identifier: GPL-3.0-or-later
add_library(dsperate_core STATIC
  nds.cpp
  mem/page_table.cpp
  mem/bus.cpp
  sched/scheduler.cpp
  cpu/cpu.cpp
  cpu/interp/interp.cpp
  gpu/gpu.cpp
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
