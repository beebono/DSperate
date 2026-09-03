# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(DSperate VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(DSPERATE_JIT      "Build the recompiler (AArch64 and ARMv7 hosts)"          ON)
option(DSPERATE_NEON     "Build NEON renderer kernels (AArch64 or ARMv7+NEON)"    ON)
option(DSPERATE_A32_SUBSET "Force the ARMv7 NEON subset on an AArch64 host (a test)" OFF)
option(DSPERATE_TESTS    "Build unit tests"                                       ON)
option(DSPERATE_HEADLESS "Build the headless frontend"                            ON)
option(DSPERATE_SDL      "Build the SDL2 frontend (off when SDL2 is not found)"   ON)
option(DSPERATE_WAYLAND  "Build the Wayland dmabuf scanout tier (off when SDL2 has no Wayland)" ON)

# Host tiers.  These are two independent questions and the build keeps them
# apart: the recompiler emits A64 and so needs an AArch64 host, while the
# renderer kernels need only *a* NEON unit.  A 32-bit ARMv7 host therefore gets
# vector kernels with the interpreter behind them, which used to be impossible
# because a single aarch64 test gated both.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(DSPERATE_HOST_AARCH64 ON)
  set(DSPERATE_HOST_ARM32 OFF)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv7l|armv7a|armhf)$")
  set(DSPERATE_HOST_AARCH64 OFF)
  set(DSPERATE_HOST_ARM32 ON)
else()
  set(DSPERATE_HOST_AARCH64 OFF)
  set(DSPERATE_HOST_ARM32 OFF)
endif()

if(NOT DSPERATE_HOST_AARCH64 AND NOT DSPERATE_HOST_ARM32 AND DSPERATE_JIT)
  message(STATUS "DSperate: host is ${CMAKE_SYSTEM_PROCESSOR}; JIT disabled (interpreter only)")
  set(DSPERATE_JIT OFF)
endif()

# On ARMv7 the kernels are built from the ARMv7-expressible NEON subset
# (gpu/neon_compat.h).  ARMv7 NEON is not IEEE-clean for single precision in
# its default mode, so ask for the VFPv4 unit and full float ABI rather than
# leaving the compiler to pick a baseline that would quietly change results.
if(DSPERATE_HOST_ARM32 AND DSPERATE_NEON)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-mfpu=neon-vfpv4 -mfloat-abi=hard")
  check_cxx_source_compiles("#include <arm_neon.h>
    int main() { return vgetq_lane_u8(vdupq_n_u8(0), 0); }" DSPERATE_ARM32_NEON_OK)
  unset(CMAKE_REQUIRED_FLAGS)
  if(DSPERATE_ARM32_NEON_OK)
    add_compile_options(-mfpu=neon-vfpv4 -mfloat-abi=hard)
    message(STATUS "DSperate: ARMv7 host; NEON kernels built from the A32 subset")
  else()
    message(STATUS "DSperate: ARMv7 host without usable NEON; ref kernels only")
    set(DSPERATE_NEON OFF)
  endif()
elseif(NOT DSPERATE_HOST_AARCH64 AND DSPERATE_NEON)
  set(DSPERATE_NEON OFF)
endif()

foreach(cfg RELEASE RELWITHDEBINFO)
  string(REPLACE "-O2" "-O3" CMAKE_C_FLAGS_${cfg}   "${CMAKE_C_FLAGS_${cfg}}")
  string(REPLACE "-O2" "-O3" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()

add_compile_options(-fstack-protector)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(src/core)
if(DSPERATE_HEADLESS)
  add_subdirectory(src/frontend/headless)
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
    # The dmabuf tier reads SDL_SysWMinfo's wl member, which SDL_syswm.h only
    # declares when SDL2 itself was built with its Wayland driver.  The
    # handheld SDL2s are fbdev/KMSDRM builds with none, so the tier is dropped
    # there at configure time rather than failing to compile.  libwayland is
    # never linked either way (wl_dyn.h dlopens it).
    if(DSPERATE_WAYLAND)
      include(CheckCXXSourceCompiles)
      set(CMAKE_REQUIRED_INCLUDES "${SDL2_INCLUDE_DIRS}")
      if(TARGET SDL2::SDL2)
        get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        if(_sdl2_inc)
          list(APPEND CMAKE_REQUIRED_INCLUDES ${_sdl2_inc})
        endif()
      endif()
      check_cxx_source_compiles("#include <SDL2/SDL_syswm.h>
        int main() { SDL_SysWMinfo i; return i.info.wl.display ? 1 : 0; }" DSPERATE_SDL2_HAS_WAYLAND)
      unset(CMAKE_REQUIRED_INCLUDES)
      if(NOT DSPERATE_SDL2_HAS_WAYLAND)
        message(STATUS "DSperate: SDL2 built without Wayland; the dmabuf scanout tier is not built")
        set(DSPERATE_WAYLAND OFF)
      endif()
    endif()
    add_subdirectory(src/frontend/sdl)
  else()
    message(STATUS "DSperate: SDL2 not found; the SDL frontend is not built")
  endif()
endif()
if(DSPERATE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
