#!/usr/bin/env bash
# Build Kasumi.vpk for the PS Vita (from devkitPro MSYS or any bash with
# CMake). First time: tools/build-transport-vita.sh and
# tools/build-aecm-vita.sh, and `vdpm curl opus`.
set -euo pipefail
cd "$(dirname "$0")/.."
export VITASDK="${VITASDK:-/c/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
cmake -S vita -B build-vita -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=FALSE
cmake --build build-vita -j8
cp build-vita/Kasumi.vpk Kasumi.vpk
echo "built Kasumi.vpk"
