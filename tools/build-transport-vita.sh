#!/usr/bin/env bash
# The PS Vita's transport: mbedTLS (DTLS), libsrtp and libpeer, the same
# vendored sources as tools/build-transport.sh, into build-transport-vita/.
# Run once (from devkitPro MSYS or any bash with CMake and VitaSDK); the
# Vita build (vita/CMakeLists.txt) links the result. Unlike the 3DS, the
# Vita's libcurl uses OpenSSL, so mbedTLS needs no renamed symbols here.
set -euo pipefail
cd "$(dirname "$0")/.."
export VITASDK="${VITASDK:-/c/vitasdk}"
cmake -S transport -B build-transport-vita -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=FALSE \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-transport-vita -j4
mkdir -p build-transport-vita/dist
cp build-transport-vita/mbedtls/library/libmbed*.a build-transport-vita/libpeer.a \
   build-transport-vita/srtp/libsrtp2.a build-transport-vita/dist/
echo "built build-transport-vita/dist"
