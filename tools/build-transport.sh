#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S transport -B build-transport -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE="$(pwd)/tools/transport-toolchain.cmake" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-transport -j4
bin=/c/devkitPro/devkitARM/bin
mkdir -p build-transport/dist
# Keep the DTLS-enabled library ABI separate from portlibs curl's mbedTLS ABI.
# Rename definitions AND references, including libpeer's references. Do not
# prefix libc or other undefined platform symbols.
"$bin/arm-none-eabi-nm" -g --defined-only build-transport/mbedtls/library/libmbed*.a |
  awk 'NF == 3 && ($3 ~ /^mbedtls_/ || $3 ~ /^psa_/ || $3 ~ /^mbedcrypto_/) {print $3 " onow_" $3}' |
  sort -u > build-transport/crypto-symbols.txt
for lib in mbedtls mbedx509 mbedcrypto; do
  "$bin/arm-none-eabi-objcopy" --redefine-syms=build-transport/crypto-symbols.txt \
    "build-transport/mbedtls/library/lib$lib.a" "build-transport/dist/libonow_$lib.a"
done
"$bin/arm-none-eabi-objcopy" --redefine-syms=build-transport/crypto-symbols.txt \
  build-transport/libpeer.a build-transport/dist/libpeer.a
cp build-transport/srtp/libsrtp2.a build-transport/dist/libsrtp2.a
