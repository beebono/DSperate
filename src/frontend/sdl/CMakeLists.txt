# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate main.cpp config.cpp display.cpp audio.cpp input.cpp menu.cpp lid.cpp mic_alsa.cpp loader_cart.cpp dmaheap.cpp
  display_drm.cpp display_disp.cpp display_fbdev.cpp)
target_link_libraries(dsperate PRIVATE dsperate_core ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})
target_include_directories(dsperate PRIVATE ${SDL2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
# The Wayland dmabuf tier (DSPERATE_WAYLAND, decided at the top level): the
# real DmabufOut with the vendored protocol code, or a stub whose open() says
# no so Display takes the paths it would have taken at runtime anyway.
if(DSPERATE_WAYLAND)
  target_sources(dsperate PRIVATE display_wl.cpp wl_dyn.cpp wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c)
  target_compile_definitions(dsperate PRIVATE DSPERATE_WAYLAND=1)
  # The vendored protocol code must marshal through the dlopen'd pointers, so
  # the macro layer is force-included (see wl/README.md); no linked wayland.
  set_source_files_properties(wl/wayland-protocol.c wl/linux-dmabuf-v1.c wl/xdg-shell.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/wl_dyn.h;-I${CMAKE_CURRENT_SOURCE_DIR}/wl")
else()
  target_sources(dsperate PRIVATE display_wl_stub.cpp)
endif()
