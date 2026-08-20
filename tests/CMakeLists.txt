# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_page_table page_table_test.cpp)
target_link_libraries(test_page_table PRIVATE dsperate_core)
add_test(NAME page_table COMMAND test_page_table)

add_executable(test_scheduler scheduler_test.cpp)
target_link_libraries(test_scheduler PRIVATE dsperate_core)
add_test(NAME scheduler COMMAND test_scheduler)
