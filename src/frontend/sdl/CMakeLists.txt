# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate-sdl main.cpp display.cpp audio.cpp input.cpp lid.cpp mic_alsa.cpp
  display_wl.cpp wl_dyn.cpp wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c)
target_link_libraries(dsperate-sdl PRIVATE dsperate_core ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})
target_include_directories(dsperate-sdl PRIVATE ${SDL2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
# The vendored protocol code must marshal through the dlopen'd pointers, so
# the macro layer is force-included (see wl/README.md); no linked wayland.
set_source_files_properties(wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c PROPERTIES
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/wl_dyn.h;-I${CMAKE_CURRENT_SOURCE_DIR}/wl")
