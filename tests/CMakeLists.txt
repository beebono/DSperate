# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_page_table page_table_test.cpp)
target_link_libraries(test_page_table PRIVATE dsperate_core)
add_test(NAME page_table COMMAND test_page_table)

add_executable(test_scheduler scheduler_test.cpp)
target_link_libraries(test_scheduler PRIVATE dsperate_core)
add_test(NAME scheduler COMMAND test_scheduler)

add_executable(test_interp interp_test.cpp)
target_link_libraries(test_interp PRIVATE dsperate_core)
add_test(NAME interp COMMAND test_interp)

add_executable(test_gpu gpu_test.cpp)
target_link_libraries(test_gpu PRIVATE dsperate_core)
add_test(NAME gpu COMMAND test_gpu)

add_executable(test_io io_test.cpp)
target_link_libraries(test_io PRIVATE dsperate_core)
add_test(NAME io COMMAND test_io)

add_executable(test_gpu3d gpu3d_test.cpp)
target_link_libraries(test_gpu3d PRIVATE dsperate_core)
add_test(NAME gpu3d COMMAND test_gpu3d)

add_executable(test_kernels kernels_test.cpp)
target_link_libraries(test_kernels PRIVATE dsperate_core)
add_test(NAME kernels COMMAND test_kernels)

if(DSPERATE_JIT)
  add_executable(test_jit jit_test.cpp)
  target_link_libraries(test_jit PRIVATE dsperate_core)
  add_test(NAME jit COMMAND test_jit)
endif()

add_executable(test_spu spu_test.cpp)
target_link_libraries(test_spu PRIVATE dsperate_core)
add_test(NAME spu COMMAND test_spu)
