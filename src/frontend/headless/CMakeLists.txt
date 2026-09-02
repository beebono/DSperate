# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate-headless main.cpp)
target_link_libraries(dsperate-headless PRIVATE dsperate_core)
