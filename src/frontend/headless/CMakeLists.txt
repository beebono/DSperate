# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate-headless main.cpp)
target_link_libraries(dsperate-headless PRIVATE dsperate_core)
# The 3D rasteriser alone on save-stated frames, for device timing of kernels.
add_executable(dsperate-raster-bench ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/raster_bench.cpp)
target_link_libraries(dsperate-raster-bench PRIVATE dsperate_core)
# The LAN transport, so two headless instances (or one and a melonDS) can
# play local wireless: the test bench for the Wi-Fi block.
if(DSPERATE_NET)
  target_link_libraries(dsperate-headless PRIVATE dsperate_net)
  target_compile_definitions(dsperate-headless PRIVATE DSPERATE_NET=1)
endif()
# The update CDN download of a DSiWare title's signed TMD (--dsi-install)
# goes through the same dlopen'd libcurl RetroAchievements uses.
if(DSPERATE_CHEEVOS)
  target_link_libraries(dsperate-headless PRIVATE dsperate_cheevos)
  target_compile_definitions(dsperate-headless PRIVATE DSPERATE_CHEEVOS=1)
endif()
