# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(dsperate-headless main.cpp)
target_link_libraries(dsperate-headless PRIVATE dsperate_core)
# The LAN transport, so two headless instances (or one and a melonDS) can
# play local wireless: the test bench for the Wi-Fi block.
if(DSPERATE_NET)
  target_link_libraries(dsperate-headless PRIVATE dsperate_net)
  target_compile_definitions(dsperate-headless PRIVATE DSPERATE_NET=1)
endif()
