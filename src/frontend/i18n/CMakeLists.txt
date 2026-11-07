# SPDX-License-Identifier: GPL-3.0-or-later
# The frontend's translations (i18n.h). Every po/*.po is linked in: the glob
# reconfigures when one is added, and builtin_po.cpp is rewritten when one
# changes.
file(GLOB DSPERATE_PO CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/po/*.po)
list(SORT DSPERATE_PO)
string(REPLACE ";" "|" _po_list "${DSPERATE_PO}")
set(_po_cpp ${CMAKE_CURRENT_BINARY_DIR}/builtin_po.cpp)
add_custom_command(OUTPUT ${_po_cpp}
  COMMAND ${CMAKE_COMMAND} "-DOUT=${_po_cpp}" "-DFILES=${_po_list}" -P ${PROJECT_SOURCE_DIR}/cmake/embed_po.cmake
  DEPENDS ${DSPERATE_PO} ${PROJECT_SOURCE_DIR}/cmake/embed_po.cmake
  COMMENT "Embedding translations" VERBATIM)
add_library(dsperate_i18n STATIC i18n.cpp ${_po_cpp})
target_include_directories(dsperate_i18n PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
