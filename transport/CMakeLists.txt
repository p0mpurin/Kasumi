cmake_minimum_required(VERSION 3.16)
project(opennow_transport VERSION 2.4.2 LANGUAGES C)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(TEST_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
add_subdirectory(../vendor/mbedtls mbedtls)
target_sources(mbedcrypto PRIVATE entropy_3ds.c)
add_subdirectory(../vendor/libsrtp srtp)
# libsrtp upstream assumes unsigned int == uint32_t (true on x86_64 LP64,
# false on arm-none-eabi newlib where uint32_t is unsigned long). The
# 3DS toolchain treats this as an incompatible-pointer warning; suppress
# -Werror for this vendored library only.
if(TARGET srtp2)
  target_compile_options(srtp2 PRIVATE -Wno-error=incompatible-pointer-types -Wno-error)
endif()
set(PEER_SOURCE ../vendor/libpeer/src)
add_library(peer STATIC
    ${PEER_SOURCE}/address.c ${PEER_SOURCE}/agent.c ${PEER_SOURCE}/base64.c
    ${PEER_SOURCE}/dtls_srtp.c ${PEER_SOURCE}/ice.c ${PEER_SOURCE}/peer.c
    ${PEER_SOURCE}/peer_connection.c ${PEER_SOURCE}/ports.c ${PEER_SOURCE}/rtcp.c
    ${PEER_SOURCE}/rtp.c ${PEER_SOURCE}/sctp.c ${PEER_SOURCE}/sdp.c
    ${PEER_SOURCE}/socket.c ${PEER_SOURCE}/stun.c ${PEER_SOURCE}/utils.c)
target_include_directories(peer PRIVATE ../vendor/mbedtls/include ../vendor/libsrtp/include
    ${CMAKE_CURRENT_BINARY_DIR}/include)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/srtp2)
configure_file(../vendor/libsrtp/include/srtp.h ${CMAKE_CURRENT_BINARY_DIR}/include/srtp2/srtp.h COPYONLY)
target_compile_definitions(peer PRIVATE CONFIG_MBEDTLS_2_X=1 CONFIG_DTLS_USE_ECDSA=1
    CONFIG_USE_USRSCTP=0 CONFIG_MAX_NALU_SIZE=524288 LOG_LEVEL=-1 __BYTE_ORDER=1234)
