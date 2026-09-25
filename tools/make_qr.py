#!/usr/bin/env python3
"""Write a QR code PNG for a URL, with no third-party packages.

    python tools/make_qr.py <url> <out.png>

Byte mode, error correction level M, versions 1-6 (up to 106 bytes), which
covers GitHub release download links. FBI's "Remote install > Scan QR code"
reads it to install Kasumi.cia straight from the release.
"""
import struct
import sys
import zlib

# Error correction level M: EC codewords per block, blocks, total codewords.
EC_PER_BLOCK = {1: 10, 2: 16, 3: 26, 4: 18, 5: 24, 6: 16}
BLOCKS = {1: 1, 2: 1, 3: 1, 4: 2, 5: 2, 6: 4}
TOTAL = {1: 26, 2: 44, 3: 70, 4: 100, 5: 134, 6: 172}
ECL_M_BITS = 0  # format bits for level M


def gf_mul(a, b):
    result = 0
    while b:
        if b & 1:
            result ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11D
        b >>= 1
    return result


def rs_generator(degree):
    poly = [1]
    root = 1
    for _ in range(degree):
        poly = [(c ^ gf_mul(n, root)) for c, n in zip(poly + [0], [0] + poly)]
        root = gf_mul(root, 2)
    return poly


def rs_remainder(data, degree):
    gen = rs_generator(degree)
    rem = [0] * degree
    for byte in data:
        factor = byte ^ rem[0]
        rem = rem[1:] + [0]
        for i in range(degree):
            rem[i] ^= gf_mul(gen[i + 1], factor)
    return rem


def codewords(payload):
    for version in range(1, 7):
        data_len = TOTAL[version] - EC_PER_BLOCK[version] * BLOCKS[version]
        if len(payload) + 2 <= data_len:  # 4-bit mode + 8-bit count + data
            break
    else:
        raise SystemExit("URL too long for a version 6 QR code")
    bits = [0, 1, 0, 0] + [(len(payload) >> i) & 1 for i in range(7, -1, -1)]
    for byte in payload:
        bits += [(byte >> i) & 1 for i in range(7, -1, -1)]
    capacity = data_len * 8
    bits += [0] * min(4, capacity - len(bits))
    bits += [0] * (-len(bits) % 8)
    data = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    pad = 0xEC
    while len(data) < data_len:
        data.append(pad)
        pad ^= 0xEC ^ 0x11
    blocks = BLOCKS[version]
    size = data_len // blocks
    split = [data[i * size:(i + 1) * size] for i in range(blocks)]
    ecc = [rs_remainder(block, EC_PER_BLOCK[version]) for block in split]
    out = [block[i] for i in range(size) for block in split]
    out += [block[i] for i in range(EC_PER_BLOCK[version]) for block in ecc]
    return version, out


def build(payload):
    version, words = codewords(payload)
    n = version * 4 + 17
    modules = [[False] * n for _ in range(n)]
    function = [[False] * n for _ in range(n)]

    def put(x, y, dark):
        modules[y][x] = dark
        function[y][x] = True

    for i in range(n):  # timing patterns
        put(6, i, i % 2 == 0)
        put(i, 6, i % 2 == 0)
    for cx, cy in ((3, 3), (n - 4, 3), (3, n - 4)):  # finders + separators
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < n and 0 <= y < n:
                    dist = max(abs(dx), abs(dy))
                    put(x, y, dist not in (2, 4))
    if version >= 2:  # one alignment pattern for versions 2-6
        c = n - 7
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                put(c + dx, c + dy, max(abs(dx), abs(dy)) != 1)
    # Reserve format areas (drawn per mask below) and the dark module.
    for i in range(9):
        function[8][i] = function[i][8] = True
    for i in range(8):
        function[8][n - 1 - i] = function[n - 1 - i][8] = True
    put(8, n - 8, True)

    # Data, in the standard zig-zag from the bottom-right corner.
    bit_index = 0
    total_bits = len(words) * 8
    right = n - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(n):
            for j in range(2):
                x = right - j
                upward = ((right + 1) & 2) == 0
                y = n - 1 - vert if upward else vert
                if not function[y][x] and bit_index < total_bits:
                    modules[y][x] = (words[bit_index >> 3] >> (7 - (bit_index & 7))) & 1 == 1
                    bit_index += 1
        right -= 2

    masks = [
        lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
        lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
        lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
        lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0,
    ]

    def with_mask(mask):
        grid = [row[:] for row in modules]
        for y in range(n):
            for x in range(n):
                if not function[y][x] and masks[mask](x, y):
                    grid[y][x] = not grid[y][x]
        data = (ECL_M_BITS << 3) | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = ((data << 10) | rem) ^ 0x5412
        bit = lambda i: (bits >> i) & 1 == 1

        def fmt(x, y, dark):
            grid[y][x] = dark
        for i in range(6):
            fmt(8, i, bit(i))
        fmt(8, 7, bit(6))
        fmt(8, 8, bit(7))
        fmt(7, 8, bit(8))
        for i in range(9, 15):
            fmt(14 - i, 8, bit(i))
        for i in range(8):
            fmt(n - 1 - i, 8, bit(i))
        for i in range(8, 15):
            fmt(8, n - 15 + i, bit(i))
        fmt(8, n - 8, True)
        return grid

    def penalty(grid):
        score = 0
        for lines in (grid, [list(col) for col in zip(*grid)]):
            for line in lines:
                run, colour = 0, None
                for cell in line:
                    if cell == colour:
                        run += 1
                    else:
                        if run >= 5:
                            score += run - 2
                        run, colour = 1, cell
                if run >= 5:
                    score += run - 2
                text = "".join("1" if c else "0" for c in line)
                score += 40 * (text.count("10111010000") + text.count("00001011101"))
        for y in range(n - 1):
            for x in range(n - 1):
                if grid[y][x] == grid[y][x + 1] == grid[y + 1][x] == grid[y + 1][x + 1]:
                    score += 3
        dark = sum(sum(row) for row in grid)
        score += abs(dark * 20 - n * n * 10) // (n * n) * 10
        return score

    return min((with_mask(m) for m in range(8)), key=penalty)


def write_png(grid, path, scale=8, border=4):
    n = len(grid)
    width = (n + border * 2) * scale
    rows = []
    for y in range(-border, n + border):
        line = bytearray([0])  # filter: none
        for x in range(-border, n + border):
            dark = 0 <= x < n and 0 <= y < n and grid[y][x]
            line += bytes([0 if dark else 255]) * scale
        rows += [bytes(line)] * scale

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, width, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    write_png(build(sys.argv[1].encode("utf-8")), sys.argv[2])
