# SPDX-License-Identifier: GPL-3.0-or-later
#
# RetroAchievements support. A library of its own rather than part of
# dsperate_core or of the SDL frontend, for two reasons: the core must stay
# free of this (it will grow a network transport in phase 3, and the headless
# frontend should never gain one), and the tests link frontend translation
# units one at a time, which a library makes straightforward.
#
# Phase 3 (docs/retroachievements-scoping.md) builds the whole of rcheevos we
# use: the hash, the condition runtime, the session (rc_client.c) and the server
# protocol (rapi/). Still not built: rc_client_raintegration.c (a Windows-only
# authoring toolkit) and rc_libretro.c (libretro's own integration).
set(RC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos)

# The condition evaluator. Whole-directory on purpose: these translation units
# reference each other freely (trigger -> condset -> condition -> operand ->
# memref) and picking a subset would be guesswork that breaks on an update.
file(GLOB RC_RUNTIME_SOURCES ${RC_DIR}/src/rcheevos/*.c)

# The session and the server protocol (phase 3).
file(GLOB RC_API_SOURCES ${RC_DIR}/src/rapi/*.c)

add_library(dsperate_cheevos STATIC
  cheevos_hash.cpp
  cheevos_memory.cpp
  cheevos_http.cpp
  cheevos_client.cpp
  ${RC_DIR}/src/rc_client.c
  ${RC_API_SOURCES}
  ${RC_RUNTIME_SOURCES}
  ${RC_DIR}/src/rhash/hash.c
  ${RC_DIR}/src/rhash/hash_rom.c
  ${RC_DIR}/src/rhash/md5.c
  ${RC_DIR}/src/rc_util.c
  ${RC_DIR}/src/rc_compat.c
  ${RC_DIR}/src/rc_version.c
)

target_include_directories(dsperate_cheevos
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
  PRIVATE ${RC_DIR}/include ${RC_DIR}/src ${RC_DIR}/src/rhash)

# What is compiled out, and why it is safe to compile out. The disc, encrypted
# (3DS) and zip hash paths are all for consoles we will never ask about, and
# dropping them drops cdreader.c, aes.c, hash_disc.c, hash_encrypted.c and
# hash_zip.c. RC_HASH_NO_ZIP in particular costs us nothing even though we do
# support zipped ROMs: a zipped ROM reaches rcheevos through our own RomSource
# (cart/zip.cpp inflated or mapped it already), so rcheevos never sees an
# archive. See cheevos_hash.cpp.
target_compile_definitions(dsperate_cheevos PRIVATE
  RC_HASH_NO_DISC RC_HASH_NO_ENCRYPTED RC_HASH_NO_ZIP RC_DISABLE_LUA)
# rc_client.c needs its own headers, and cheevos_client.cpp includes rc_client.h.
target_include_directories(dsperate_cheevos PRIVATE ${RC_DIR}/src/rapi)

# The version in the User-Agent DSperate sends to RetroAchievements. The server
# refuses a client it cannot recognise (403 unsupported_client) on every
# endpoint, so this string is load-bearing -- see cheevos_memory.h. Taken from
# the latest release tag, because project() VERSION in the top-level
# CMakeLists.txt is stale (1.0.0); the literal is the fallback for a build from
# a tarball or a checkout with no tags, and wants bumping with each release.
set(DSPERATE_CHEEVOS_VERSION "" CACHE STRING "Version DSperate reports to RetroAchievements (default: latest git tag)")
if(NOT DSPERATE_CHEEVOS_VERSION)
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --abbrev=0
                  OUTPUT_VARIABLE _ds_tag OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _ds_tag_rc)
  if(_ds_tag_rc EQUAL 0 AND _ds_tag MATCHES "^v?([0-9]+\\.[0-9]+(\\.[0-9]+)?)$")
    set(DSPERATE_CHEEVOS_VERSION "${CMAKE_MATCH_1}")
  else()
    set(DSPERATE_CHEEVOS_VERSION "1.13.1")
  endif()
endif()
message(STATUS "DSperate: RetroAchievements client version ${DSPERATE_CHEEVOS_VERSION}")
target_compile_definitions(dsperate_cheevos PRIVATE
  DSPERATE_CHEEVOS_VERSION="${DSPERATE_CHEEVOS_VERSION}")

# For cart::RomSource. The dependency runs this way round on purpose: the core
# knows nothing about achievements.
target_link_libraries(dsperate_cheevos PUBLIC dsperate_core ${CMAKE_DL_LIBS})

# Prints the hash for the ROMs named on the command line, which is how the
# result gets checked against what RetroAchievements actually has. Built with
# the library because it is the only way to exercise this before the session
# code of phase 3 exists.
add_executable(dsperate_cheevos_hash ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cheevos_hash.cpp)
target_link_libraries(dsperate_cheevos_hash PRIVATE dsperate_cheevos)

# What the condition runtime costs per frame, which has to be known on an A55
# before phase 3 commits to a shape. Reaches rcheevos' headers directly, as the
# test does.
# Drives a real session from the command line: the only way to exercise login
# and set loading before there is any UI.
add_executable(dsperate_cheevos_session ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cheevos_session.cpp)
target_link_libraries(dsperate_cheevos_session PRIVATE dsperate_cheevos)

add_executable(dsperate_cheevos_bench ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cheevos_bench.cpp)
target_link_libraries(dsperate_cheevos_bench PRIVATE dsperate_cheevos)
target_include_directories(dsperate_cheevos_bench PRIVATE ${RC_DIR}/include)
