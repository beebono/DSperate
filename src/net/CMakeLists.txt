# SPDX-License-Identifier: GPL-3.0-or-later
#
# Networking: the transports behind the Wi-Fi block's MpTransport and
# NetDriver (core/io/wifi_transport.h). A library of its own so the core
# stays socket-free and so a frontend picks it up only when built with
# DSPERATE_NET. docs/wifi-scoping.md.
#
# enet/ is vendored (enet/README.md). Its own CMake probes these before
# building; the same probes here, so unix.c compiles the way upstream means
# it to on every tier (glibc 2.23 on the A30 has all of them).
include(CheckFunctionExists)
include(CheckStructHasMember)
include(CheckTypeSize)
check_function_exists("fcntl" ENET_HAS_FCNTL)
check_function_exists("poll" ENET_HAS_POLL)
check_function_exists("getaddrinfo" ENET_HAS_GETADDRINFO)
check_function_exists("getnameinfo" ENET_HAS_GETNAMEINFO)
check_function_exists("gethostbyname_r" ENET_HAS_GETHOSTBYNAME_R)
check_function_exists("gethostbyaddr_r" ENET_HAS_GETHOSTBYADDR_R)
check_function_exists("inet_pton" ENET_HAS_INET_PTON)
check_function_exists("inet_ntop" ENET_HAS_INET_NTOP)
check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" ENET_HAS_MSGHDR_FLAGS)
set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
check_type_size("socklen_t" ENET_HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
unset(CMAKE_EXTRA_INCLUDE_FILES)

set(ENET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/enet)
add_library(dsperate_enet STATIC
  ${ENET_DIR}/callbacks.c ${ENET_DIR}/compress.c ${ENET_DIR}/host.c ${ENET_DIR}/list.c
  ${ENET_DIR}/packet.c ${ENET_DIR}/peer.c ${ENET_DIR}/protocol.c ${ENET_DIR}/unix.c)
target_include_directories(dsperate_enet PUBLIC ${ENET_DIR}/include)
foreach(probe FCNTL POLL GETADDRINFO GETNAMEINFO GETHOSTBYNAME_R GETHOSTBYADDR_R INET_PTON INET_NTOP MSGHDR_FLAGS SOCKLEN_T)
  if(ENET_HAS_${probe})
    target_compile_definitions(dsperate_enet PRIVATE HAS_${probe}=1)
  endif()
endforeach()
# Third-party C: not ours to fix warnings in.
target_compile_options(dsperate_enet PRIVATE -w)

add_library(dsperate_net STATIC
  lan_mp.cpp)
target_include_directories(dsperate_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dsperate_net PUBLIC dsperate_core dsperate_enet)
