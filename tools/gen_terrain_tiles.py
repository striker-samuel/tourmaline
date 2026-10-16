#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

"""Synthetic tiled-terrain source generator (T0 test data).

Writes, with stdlib only (no PIL):
  out/ terrain.json + h_XX_ZZ.png (16x, 1024 RGBA8, RG16 height, B/A=0)
                  + m_XX_ZZ.png (256x, 256 RGBA8, checker-in-R test mask)

World: 4096x4096m, origin (-2048,-2048). H-tile 1024m, G-block 256m.
Mapping: worldY = -40 + h01*420. Synthetic hills (deterministic):
  h01 = clamp01(0.55 + 0.18*sin(x*0.004)*cos(z*0.0035) + 0.08*sin(x*0.017+1.7)*sin(z*0.013+0.6))
Checker: 32px squares in R, G/B/A = 0.

Usage: python3 tools/gen_terrain_tiles.py --out content/terrain_src
"""
import argparse
import json
import math
import os
import struct
import zlib

BASE_Y = -40.0
RANGE = 420.0
WORLD = 4096.0
ORIGIN = -2048.0
H_TILES = 4
H_RES = 1024
H_SIZE = 1024.0
S_RES = 256


def write_png_rgba(path, w, h, rows):
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rows[y] for y in range(h))
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def synth_h01(x, z):
    v = (
        0.55
        + 0.18 * math.sin(x * 0.004) * math.cos(z * 0.0035)
        + 0.08 * math.sin(x * 0.017 + 1.7) * math.sin(z * 0.013 + 0.6)
    )
    return min(1.0, max(0.0, v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="content/terrain_src")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for tz in range(H_TILES):
        for tx in range(H_TILES):
            ox = ORIGIN + tx * H_SIZE
            oz = ORIGIN + tz * H_SIZE
            rows = []
            for py in range(H_RES):
                # py=0 is tile-local -z edge (matches ComputeNormalH D/U convention).
                z = oz + (py + 0.5) * (H_SIZE / H_RES)
                row = bytearray()
                for px in range(H_RES):
                    x = ox + (px + 0.5) * (H_SIZE / H_RES)
                    v = int(synth_h01(x, z) * 65535.0 + 0.5)
                    row += bytes((v >> 8 & 0xFF, v & 0xFF, 0, 0))
                rows.append(bytes(row))
            write_png_rgba(os.path.join(args.out, f"h_{tx:02d}_{tz:02d}.png"), H_RES, H_RES, rows)

    g = 16  # 16x16 S-tiles of 256m
    for tz in range(g):
        for tx in range(g):
            rows = []
            for py in range(S_RES):
                row = bytearray()
                for px in range(S_RES):
                    # World-continuous 32px checker in R.
                    wx = tx * S_RES + px
                    wy = tz * S_RES + py
                    r = 255 if ((wx // 32) + (wy // 32)) % 2 == 0 else 0
                    row += bytes((r, 0, 0, 255))
                rows.append(bytes(row))
            write_png_rgba(os.path.join(args.out, f"m_{tx:02d}_{tz:02d}.png"), S_RES, S_RES, rows)

    meta = {
        "baseY": BASE_Y,
        "range": RANGE,
        "worldSize": WORLD,
        "origin": [ORIGIN, ORIGIN],
        "hTileSize": H_SIZE,
        "hRes": H_RES,
        "gBlock": 256.0,
        "packing": "rg16",
    }
    with open(os.path.join(args.out, "terrain.json"), "w") as f:
        json.dump(meta, f, indent=2)
    print(f"wrote 16 H + {g*g} S tiles to {args.out}")


if __name__ == "__main__":
    main()
