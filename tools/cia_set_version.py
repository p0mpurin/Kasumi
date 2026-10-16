#!/usr/bin/env python3
"""Write the title version into a CIA's ticket and TMD.

    python tools/cia_set_version.py Kasumi.cia MAJOR MINOR MICRO

The makerom build we use drops the minor number (1.1.0 came out as 1.0.0),
and the 3DS then refuses the CIA as older than the installed one (AM error
0xD8E08027). makerom's signatures are fake, so patching the field is safe.
"""
import struct
import sys

SIGNATURE_SIZES = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}


def align(value):
    return (value + 63) & ~63


def body_offset(data, offset):
    kind = struct.unpack_from('>I', data, offset)[0]
    return offset + 4 + SIGNATURE_SIZES[kind]


def main():
    path, major, minor, micro = sys.argv[1], *map(int, sys.argv[2:5])
    if not (0 <= major <= 63 and 0 <= minor <= 63 and 0 <= micro <= 15):
        sys.exit('version out of range: major and minor 0-63, micro 0-15')
    version = (major << 10) | (minor << 4) | micro

    data = bytearray(open(path, 'rb').read())
    header, _, _, certs, ticket, tmd = struct.unpack_from('<IHHIII', data, 0)
    ticket_at = align(header) + align(certs)
    tmd_at = ticket_at + align(ticket)
    struct.pack_into('>H', data, body_offset(data, ticket_at) + 0xA6, version)
    struct.pack_into('>H', data, body_offset(data, tmd_at) + 0x9C, version)
    open(path, 'wb').write(data)
    print(f'{path}: title version {major}.{minor}.{micro} ({version})')


if __name__ == '__main__':
    main()
