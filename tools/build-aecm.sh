#!/usr/bin/env bash
# Build WebRTC's mobile echo canceller (vendor/webrtc-aecm, BSD licence, the
# subset TriCord ships) into build-aecm/libaecm.a for voice chat. Run once,
# like tools/build-transport.sh; the Makefile links the result.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=/opt/devkitpro/devkitARM/bin
arch="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
src=vendor/webrtc-aecm
out=build-aecm
mkdir -p "$out/obj"
rm -f "$out"/obj/*.o "$out/libaecm.a"
flags="-O2 -g -ffunction-sections -fdata-sections $arch -D__3DS__ -I$src"
for file in $(find "$src" -name '*.c'); do
    "$bin/arm-none-eabi-gcc" $flags -std=gnu11 -c "$file" -o "$out/obj/$(basename "${file%.c}").o"
done
for file in $(find "$src" -name '*.cpp'); do
    "$bin/arm-none-eabi-g++" $flags -std=gnu++17 -fno-exceptions -fno-rtti -c "$file" \
        -o "$out/obj/$(basename "${file%.cpp}").o"
done
"$bin/arm-none-eabi-ar" rcs "$out/libaecm.a" "$out"/obj/*.o
echo "built $out/libaecm.a"
