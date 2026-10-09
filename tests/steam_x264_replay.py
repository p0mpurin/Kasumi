"""Replay real x264 slice data through Kasumi's Steam frame assembler.

Requires ffmpeg on PATH and the executable built by tools/test-steam-video.cmd.
All generated files live in ignored build-host/steam-video.
"""
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build-host" / "steam-video"
OUT.mkdir(parents=True, exist_ok=True)


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


def rbsp(nal):
    result = bytearray()
    zeros = 0
    for value in nal:
        if zeros == 2 and value == 3:
            zeros = 0
            continue
        result.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return bytes(result)


def first_mb(nal):
    bits = "".join(f"{value:08b}" for value in rbsp(nal)[1:])
    zeros = len(bits) - len(bits.lstrip("0"))
    return int(bits[zeros:zeros * 2 + 1], 2) - 1


source = OUT / "x264-main-four-slices.h264"
run("ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
    "testsrc2=size=800x450:rate=30", "-frames:v", "60", "-c:v", "libx264",
    "-profile:v", "main", "-level:v", "2.2", "-preset", "veryfast", "-tune", "zerolatency",
    "-b:v", "1400k", "-maxrate", "1400k", "-bufsize", "1400k", "-x264-params",
    "slices=4:threads=4:ref=1:bframes=0:aud=1:keyint=30:scenecut=0", "-f", "h264", source)

nals = [x for x in re.split(b"\x00\x00\x00?\x01", source.read_bytes()) if x]
frames = []
for nal in nals:
    if nal[0] & 31 == 9:
        frames.append([])
    else:
        frames[-1].append(nal)
assert len(frames) == 60
expected = b"".join(b"\0\0\0\1" + nal for frame in frames for nal in frame)
golden = OUT / "x264-expected.h264"
golden.write_bytes(expected)

for label, order in (("ordered", (0, 1, 2, 3)), ("reordered", (0, 3, 2, 1))):
    packets = OUT / f"x264-{label}.packets"
    sequence = 65500
    with packets.open("wb") as output:
        for frame_index, frame in enumerate(frames):
            slices = [nal for nal in frame if nal[0] & 31 in (1, 5)]
            prefix = [nal for nal in frame if nal[0] & 31 not in (1, 5)]
            assert len(slices) == 4
            starts = [first_mb(nal) for nal in slices]
            ends = [x - 1 for x in starts[1:]] + [50 * 29 - 1]
            parts = [(nal, 0, ends[0], 0) for nal in prefix]
            parts += [(slices[i], starts[i], ends[i], 4 | (8 if i == 3 else 0)) for i in order]
            keyframe = slices[0][0] & 31 == 5
            for part_index, (nal, first, last, flags) in enumerate(parts):
                flags |= 3  # Add Annex-B prefix and emulation prevention.
                if keyframe and part_index == 0:
                    flags |= 16
                message = (b"\1" + struct.pack("<H", (65525 + frame_index) & 65535) + bytes(10)
                           + struct.pack("<HBHH", sequence & 65535, flags, first, last) + rbsp(nal))
                output.write(struct.pack("<I", len(message)) + message)
                sequence += 1
    assembled = OUT / f"x264-{label}.h264"
    run(ROOT / "build-host" / "steam_video_test.exe", "replay", packets, assembled)
    assert assembled.read_bytes() == expected, f"{label}: lost, duplicated or misordered NAL bytes"
    run("ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-err_detect", "explode", "-y",
        "-i", assembled, "-f", "framemd5", OUT / f"x264-{label}.md5")

assert (OUT / "x264-ordered.md5").read_bytes() == (OUT / "x264-reordered.md5").read_bytes()
print("PASS: 60 x264 Main frames, four slices each; ordered and reordered streams decode identically")
