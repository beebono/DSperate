# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(DSperate VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(DSPERATE_JIT      "Build the ARM->AArch64 recompiler (AArch64 hosts only)" ON)
option(DSPERATE_NEON     "Build NEON renderer kernels (AArch64 hosts only)"       ON)
option(DSPERATE_TESTS    "Build unit tests"                                       ON)
option(DSPERATE_CLI      "Build the CLI frontend"                                 ON)
option(DSPERATE_SDL      "Build the SDL2 frontend (off when SDL2 is not found)"   ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(DSPERATE_HOST_AARCH64 ON)
else()
  set(DSPERATE_HOST_AARCH64 OFF)
  if(DSPERATE_JIT)
    message(STATUS "DSperate: host is ${CMAKE_SYSTEM_PROCESSOR}; JIT disabled (interpreter only)")
    set(DSPERATE_JIT OFF)
  endif()
  if(DSPERATE_NEON)
    set(DSPERATE_NEON OFF)
  endif()
endif()

# -O3 in place of CMake's default -O2 for the optimised configurations.
# Measured on the RK3566 against the five replay scenes, paired interleaved
# reps: -4.35 % (mlbis), -4.53 % (meteos), -5.92 % (sm64), -6.15 % (etody),
# -7.75 % (dbori) of frame time, faster in 40 of 40 reps, frames byte-identical
# on every scene. The win is renderer-side (2D select -8 %, 3D spans -5 %,
# geometry -9 %); CPU barely moves, because the recompiler emits its own code
# and compiler flags cannot reach it. See docs/plan-cpu.md.
foreach(cfg RELEASE RELWITHDEBINFO)
  string(REPLACE "-O2" "-O3" CMAKE_C_FLAGS_${cfg}   "${CMAKE_C_FLAGS_${cfg}}")
  string(REPLACE "-O2" "-O3" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()

# The toolchain defaults to -fstack-protector-strong, which guards 462 sites in
# the core objects and costs -1.07 % (sm64) / -2.50 % (dbori) of frame time.
# Basic -fstack-protector keeps 76 of them -- the functions with actual arrays,
# which is what a malicious ROM would target -- and recovers the whole
# difference: it measured level with -fno-stack-protector (-1.02 % / -2.43 %),
# so there is no safety traded for the speed. Do not "upgrade" this to -strong.
add_compile_options(-fstack-protector)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(src/core)
if(DSPERATE_CLI)
  add_subdirectory(src/frontend/cli)
endif()
if(DSPERATE_SDL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND AND TARGET SDL2::SDL2)
      set(SDL2_LIBRARIES SDL2::SDL2)
    endif()
  endif()
  if(SDL2_FOUND)
    add_subdirectory(src/frontend/sdl)
  else()
    message(STATUS "DSperate: SDL2 not found; the SDL frontend is not built")
  endif()
endif()
if(DSPERATE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
