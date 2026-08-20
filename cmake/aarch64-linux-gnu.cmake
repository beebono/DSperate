# SPDX-License-Identifier: GPL-3.0-or-later
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Lets `ctest` run cross-built binaries on an x86 host.
find_program(QEMU_AARCH64 qemu-aarch64-static)
if(QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${QEMU_AARCH64};-L;/usr/aarch64-linux-gnu")
endif()
