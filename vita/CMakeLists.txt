# Kasumi for the PS Vita: the shared code in source/ plus the Vita's side of
# platform.h in source/vita/. Needs VitaSDK with curl and opus (vdpm), and
# the transport built once by tools/build-transport-vita.sh and
# tools/build-aecm-vita.sh. Build with tools/build-vita.sh.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Set VITASDK to the VitaSDK folder.")
  endif()
endif()

project(Kasumi C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The version comes from the Makefile, so both consoles always match.
file(STRINGS "${ROOT}/Makefile" VERSION_LINES REGEX "^VERSION_(MAJOR|MINOR|MICRO|SUFFIX) :=")
foreach(line ${VERSION_LINES})
  string(REGEX MATCH "^VERSION_([A-Z]+) :=[ ]*(.*)$" _ "${line}")
  set(VERSION_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
set(APP_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_MICRO}${VERSION_SUFFIX}")
# The VPK's own version is XX.YY: the major and the minor.
math(EXPR VPK_MAJOR "${VERSION_MAJOR}")
math(EXPR VPK_MINOR "${VERSION_MINOR}")
if(VPK_MAJOR LESS 10)
  set(VPK_MAJOR "0${VPK_MAJOR}")
endif()
if(VPK_MINOR LESS 10)
  set(VPK_MINOR "0${VPK_MINOR}")
endif()

set(VITA_APP_NAME "Kasumi")
set(VITA_TITLEID "KSMI00001")
set(VITA_VERSION "${VPK_MAJOR}.${VPK_MINOR}")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

file(GLOB SHARED_SOURCES "${ROOT}/source/*.c")
list(FILTER SHARED_SOURCES EXCLUDE REGEX "webrtc_peer\\.c$")
file(GLOB VITA_SOURCES "${ROOT}/source/vita/*.c")

# Art, sounds and the CA bundle are linked in as they are, exposing
# _binary_NAME_start / _end like the 3DS build's.
set(EMBED_OBJECTS "")
function(embed folder file)
  string(REPLACE "/" "_" flat "${file}")
  set(out "${CMAKE_CURRENT_BINARY_DIR}/embed/${flat}.o")
  add_custom_command(OUTPUT "${out}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/embed"
    COMMAND "${VITASDK}/bin/arm-vita-eabi-ld" -r -b binary -o "${out}" "${file}"
    WORKING_DIRECTORY "${folder}"
    DEPENDS "${folder}/${file}")
  set(EMBED_OBJECTS ${EMBED_OBJECTS} "${out}" PARENT_SCOPE)
endfunction()
file(GLOB GFX RELATIVE "${ROOT}/gfx" "${ROOT}/gfx/*.png")
foreach(file ${GFX})
  embed("${ROOT}/gfx" "${file}")
endforeach()
file(GLOB SOUNDS RELATIVE "${ROOT}/audio" "${ROOT}/audio/*.opus")
foreach(file ${SOUNDS})
  embed("${ROOT}/audio" "${file}")
endforeach()
embed("${ROOT}" "romfs/cacert.pem")
set_source_files_properties(${EMBED_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# jansson from source: vdpm's build is position-independent, which
# vita-elf-create can't relocate (R_ARM_BASE_PREL).
file(GLOB JANSSON_SOURCES "${ROOT}/vendor/jansson/src/*.c")
add_library(jansson_vita STATIC ${JANSSON_SOURCES})
target_compile_options(jansson_vita PRIVATE -O2 -g -ffunction-sections -fdata-sections -fno-pic)
target_compile_definitions(jansson_vita PRIVATE HAVE_CONFIG_H=1)
target_include_directories(jansson_vita PUBLIC "${ROOT}/vendor/jansson/src")

add_executable(Kasumi ${SHARED_SOURCES} ${VITA_SOURCES} ${EMBED_OBJECTS})

target_compile_options(Kasumi PRIVATE -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
  -Wno-unused-parameter -Wno-sign-compare)
target_compile_definitions(Kasumi PRIVATE APP_VERSION="${APP_VERSION}")
target_include_directories(Kasumi BEFORE PRIVATE
  "${ROOT}/vendor/jansson/src"
  "${ROOT}/include"
  "${ROOT}/vendor/libpeer/src"
  "${ROOT}/vendor/mbedtls/include"
  "${ROOT}/vendor/libsrtp/include"
  "${ROOT}/build-transport-vita/include")
target_link_directories(Kasumi PRIVATE
  "${ROOT}/build-transport-vita/dist"
  "${ROOT}/build-aecm-vita")

target_link_libraries(Kasumi
  -Wl,--start-group
  jansson_vita peer srtp2 mbedtls mbedx509 mbedcrypto aecm
  curl ssl crypto zstd opus
  vita2d png jpeg freetype harfbuzz bz2 z
  -Wl,--end-group
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub SceMotion_stub
  SceAudio_stub SceAudioIn_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
  ScePower_stub SceNet_stub SceNetCtl_stub SceRtc_stub SceVideodec_stub ScePgf_stub ScePvf_stub
  SceKernelDmacMgr_stub SceLibKernel_stub
  pthread m c stdc++)

vita_create_self(eboot.bin Kasumi UNSAFE)
vita_create_vpk(Kasumi.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
