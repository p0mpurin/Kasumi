#!/usr/bin/env bash
# The PS Vita's build of WebRTC's mobile echo canceller (see build-aecm.sh)
# into build-aecm-vita/libaecm.a. Run once; vita/CMakeLists.txt links it.
set -euo pipefail
cd "$(dirname "$0")/.."
bin="${VITASDK:-/c/vitasdk}/bin"
src=vendor/webrtc-aecm
out=build-aecm-vita
mkdir -p "$out/obj"
rm -f "$out"/obj/*.o "$out/libaecm.a"
flags="-O2 -g -ffunction-sections -fdata-sections -march=armv7-a -mfpu=neon -mfloat-abi=hard -D__vita__ -I$src"
for file in $(find "$src" -name '*.c'); do
    "$bin/arm-vita-eabi-gcc" $flags -std=gnu11 -c "$file" -o "$out/obj/$(basename "${file%.c}").o"
done
for file in $(find "$src" -name '*.cpp'); do
    "$bin/arm-vita-eabi-g++" $flags -std=gnu++17 -fno-exceptions -fno-rtti -c "$file" \
        -o "$out/obj/$(basename "${file%.cpp}").o"
done
"$bin/arm-vita-eabi-ar" rcs "$out/libaecm.a" "$out"/obj/*.o
echo "built $out/libaecm.a"
