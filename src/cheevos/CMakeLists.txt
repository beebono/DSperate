# SPDX-License-Identifier: GPL-3.0-or-later
#
# RetroAchievements support. A library of its own rather than part of
# dsperate_core or of the SDL frontend, for two reasons: the core must stay
# free of this (it will grow a network transport in phase 3, and the headless
# frontend should never gain one), and the tests link frontend translation
# units one at a time, which a library makes straightforward.
#
# Phase 1 (docs/retroachievements-scoping.md) is identity only: the
# RetroAchievements hash for a ROM, no session and no network. So only the
# hashing corner of rcheevos is compiled; rc_client.c, rapi/ and rcheevos/
# are vendored but not built until phases 2 and 3 need them.
set(RC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos)

add_library(dsperate_cheevos STATIC
  cheevos_hash.cpp
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

# For cart::RomSource. The dependency runs this way round on purpose: the core
# knows nothing about achievements.
target_link_libraries(dsperate_cheevos PUBLIC dsperate_core)

# Prints the hash for the ROMs named on the command line, which is how the
# result gets checked against what RetroAchievements actually has. Built with
# the library because it is the only way to exercise this before the session
# code of phase 3 exists.
add_executable(dsperate_cheevos_hash ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cheevos_hash.cpp)
target_link_libraries(dsperate_cheevos_hash PRIVATE dsperate_cheevos)
