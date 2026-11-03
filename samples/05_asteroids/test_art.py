#!/usr/bin/env python3
"""PNG atlas contract checks. Uses only the Python standard library."""
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent / "assets"

def check_png(name: str, expected: tuple[int, int]) -> None:
    data = (ROOT / name).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{name}: PNG signature"
    offset = 8
    seen_ihdr = seen_iend = False
    idat = bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        tag = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
        assert zlib.crc32(tag + payload) == crc, f"{name}: invalid CRC"
        if tag == b"IHDR":
            width, height, depth, kind, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload)
            assert (width, height) == expected
            assert (depth, kind, compression, filtering, interlace) == (8, 6, 0, 0, 0)
            seen_ihdr = True
        if tag == b"IDAT":
            idat.extend(payload)
        offset += size + 12
        if tag == b"IEND":
            seen_iend = True
            break
    assert seen_ihdr and seen_iend and offset == len(data)
    assert len(zlib.decompress(idat)) > expected[0] * expected[1] * 4
    print(f"{name}: RGBA8 {expected[0]}x{expected[1]} PNG valid")

check_png("sprites.png", (256, 256))
check_png("font.png", (128, 60))
