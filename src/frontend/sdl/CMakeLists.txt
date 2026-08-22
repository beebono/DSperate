# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate-sdl main.cpp display.cpp audio.cpp input.cpp)
target_link_libraries(dsperate-sdl PRIVATE dsperate_core ${SDL2_LIBRARIES})
target_include_directories(dsperate-sdl PRIVATE ${SDL2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
