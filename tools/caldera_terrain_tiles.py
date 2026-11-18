#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_terrain_tiles: convert the stitched island heightmap into the
terrain system's H-tile source (terrain.json + h_XX_ZZ.png RG16 tiles + solid
m_XX_ZZ.png masks).

The caldera_terrain_heightfield.py --stitch output is a 16-bit grayscale PNG
in the USD stage's Z-up inches (X/Y horizontal, Z = height, 32-inch texels).
The terrain system consumes a SQUARE worldSize x worldSize field in Tourmaline
Y-up meters, authored as tilesPerAxis^2 H-tiles of hRes x hRes RGBA8 (RG16
height). This tool:
  - maps USD -> Tourmaline (X_t = X_usd * mpu, Z_t = -Y_usd * mpu, the -Y flip),
  - resamples the heightmap onto the square atlas (bilinear),
  - pads the uncovered (ocean) margin with baseY,
  - emits terrain.json + h_XX_ZZ.png + solid m_XX_ZZ.png masks.

Usage:
  python tools/caldera_terrain_tiles.py \
      --heightmap G:/caldera_test/terrain_heightmaps/island.height.png \
      --meta G:/caldera_test/terrain_heightmaps/island.height.json \
      --out G:/caldera_test/terrain_src \
      [--world-size 4096] [--block 256] [--htile 1024] [--hres 1024]
"""
from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path

from PIL import Image


def write_png_rgba(path: Path, w: int, h: int, rows: list[bytes]) -> None:
    def chunk(typ: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rows[y] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_terrain_tiles")
    ap.add_argument("--heightmap", required=True, help="16-bit grayscale heightmap PNG")
    ap.add_argument("--meta", required=True, help="heightmap meta JSON")
    ap.add_argument("--out", required=True, help="terrain source output dir")
    ap.add_argument("--world-size", type=float, default=4096.0)
    ap.add_argument("--block", type=float, default=256.0)
    ap.add_argument("--htile", type=float, default=1024.0)
    ap.add_argument("--hres", type=int, default=1024)
    ap.add_argument("--coverage", help="8-bit coverage mask PNG (255=land, 0=water); "
                                      "optional, else solid masks")
    args = ap.parse_args(argv)

    meta = json.loads(Path(args.meta).read_text(encoding="utf-8"))
    img = Image.open(args.heightmap)
    # 16-bit grayscale -> uint16 array (row-major, nx fast / ny slow).
    hm = list(img.getdata())  # ints 0..65535
    nx, ny = img.size
    coverage = None
    if args.coverage:
        cov_img = Image.open(args.coverage)
        coverage = list(cov_img.getdata())  # 0..255 (255=land, 0=water)
        if cov_img.size != (nx, ny):
            print(f"caldera_terrain_tiles: coverage mask size {cov_img.size} != "
                  f"heightmap {img.size}; ignoring coverage", flush=True)
            coverage = None
    mpu = float(meta["metersPerUnit"])
    origin = meta["origin"]
    spacing = meta["spacing"]
    minz, maxz = meta["height"]

    base_y = minz * mpu
    range_y = (maxz - minz) * mpu
    world_size = args.world_size
    block = args.block
    htile = args.htile
    hres = args.hres
    tiles_per_axis = int(round(world_size / htile))
    blocks_per_axis = int(round(world_size / block))
    atlas_res = tiles_per_axis * hres

    # USD origin (inches) -> Tourmaline meters.
    #   X_t = X_usd * mpu                       (no flip)
    #   Z_t = -Y_usd * mpu                      (the -Y flip), so the heightmap's
    #   py=0 row (USD miny) is the +Z edge and py=ny-1 is the -Z edge.
    origin_x = origin[0] * mpu
    # min Tourmaline Z = -(max USD Y) = -(origin_y + (ny-1)*spacing_y) * mpu
    origin_z = -(origin[1] + (ny - 1) * spacing[1]) * mpu
    texel_world = htile / float(hres)

    # Round the world origin down to the atlas texel grid so the terrain sits
    # on integer texels (no sub-texel drift), keeping baseY/range exact.
    origin_x = (origin_x // texel_world) * texel_world
    origin_z = (origin_z // texel_world) * texel_world

    print(f"caldera_terrain_tiles: heightmap {nx}x{ny} @ {spacing[0]*mpu:.4f}m "
          f"-> world {world_size:.0f}m ({tiles_per_axis}x{tiles_per_axis} H-tiles "
          f"of {htile:.0f}m, {blocks_per_axis}x{blocks_per_axis} blocks of {block:.0f}m)")
    print(f"  baseY={base_y:.2f} range={range_y:.2f} origin=({origin_x:.1f},{origin_z:.1f})")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # Heightmap -> world sample (bilinear over the native grid, edge-clamped;
    # returns None outside the heightmap footprint = ocean pad).
    def sample_world(x: float, z: float):
        # world (Tourmaline) -> heightmap pixel, accounting for the -Y flip:
        #   hm_px = (X_t - origin_x0) / spx
        #   hm_py = (Z_t - origin_z0) / spy  (flipped: py grows as Z_t grows
        #          because py=0 is +Z and py=ny-1 is -Z)
        spx = spacing[0] * mpu
        spy = spacing[1] * mpu
        hx = (x - origin_x0) / spx
        hy = (z - origin_z0) / spy
        if hx < 0 or hy < 0 or hx > nx - 1 or hy > ny - 1:
            return None
        x0 = int(hx); y0 = int(hy)
        x1 = min(x0 + 1, nx - 1); y1 = min(y0 + 1, ny - 1)
        fx = hx - x0; fy = hy - y0
        v00 = hm[y0 * nx + x0]; v10 = hm[y0 * nx + x1]
        v01 = hm[y1 * nx + x0]; v11 = hm[y1 * nx + x1]
        return (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy

    origin_x0 = origin[0] * mpu            # USD minx -> Tourmaline min X
    origin_z0 = -(origin[1] + (ny - 1) * spacing[1]) * mpu  # min Tourmaline Z

    # Coverage -> world sample (bilinear, 0 = water/hole, 255 = land).
    def sample_coverage(x: float, z: float):
        if coverage is None:
            return 255  # no coverage mask: everything solid
        spx = spacing[0] * mpu
        spy = spacing[1] * mpu
        hx = (x - origin_x0) / spx
        hy = (z - origin_z0) / spy
        if hx < 0 or hy < 0 or hx > nx - 1 or hy > ny - 1:
            return 0  # outside the heightmap footprint = ocean
        x0 = int(hx); y0 = int(hy)
        x1 = min(x0 + 1, nx - 1); y1 = min(y0 + 1, ny - 1)
        fx = hx - x0; fy = hy - y0
        v00 = coverage[y0 * nx + x0]; v10 = coverage[y0 * nx + x1]
        v01 = coverage[y1 * nx + x0]; v11 = coverage[y1 * nx + x1]
        return (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy

    for tz in range(tiles_per_axis):
        for tx in range(tiles_per_axis):
            rows = []
            for py in range(hres):
                row = bytearray()
                # py=0 is the -Z edge (terrain_hfield.h): Z grows with py.
                z = origin_z + (tz * hres + py + 0.5) * texel_world
                for px in range(hres):
                    x = origin_x + (tx * hres + px + 0.5) * texel_world
                    v = sample_world(x, z)
                    if v is None:
                        v = 0.0  # ocean pad -> baseY (h01 = 0)
                    v16 = int(round(v))
                    row += bytes((v16 >> 8 & 0xFF, v16 & 0xFF, 0, 0))
                rows.append(bytes(row))
            write_png_rgba(out / f"h_{tx:02d}_{tz:02d}.png", hres, hres, rows)

    # S-masks: one mask tile per G-block, 256x256 RGBA8. R=255 default surface
    # blend; A = coverage (255 land / 0 water-hole) from the coverage mask, so
    # the water bodies and the ocean pad cut out (the vertex shader clips
    # A<0.5). Without a --coverage mask everything is solid.
    mres = 256
    mtexel = block / float(mres)
    for bz in range(blocks_per_axis):
        for bx in range(blocks_per_axis):
            rows = []
            for py in range(mres):
                row = bytearray()
                z = origin_z + (bz * mres + py + 0.5) * mtexel
                for px in range(mres):
                    x = origin_x + (bx * mres + px + 0.5) * mtexel
                    a = 255 if sample_coverage(x, z) >= 0.5 else 0
                    row += bytes((255, 0, 0, a))
                rows.append(bytes(row))
            write_png_rgba(out / f"m_{bx:02d}_{bz:02d}.png", mres, mres, rows)

    meta_out = {
        "baseY": base_y,
        "range": range_y,
        "worldSize": world_size,
        "origin": [origin_x, origin_z],
        "hTileSize": htile,
        "hRes": hres,
        "gBlock": block,
        "packing": "rg16",
    }
    (out / "terrain.json").write_text(json.dumps(meta_out, indent=2), encoding="utf-8")
    print(f"  wrote {tiles_per_axis*tiles_per_axis} H-tiles + {blocks_per_axis*blocks_per_axis} "
          f"masks + terrain.json -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))
