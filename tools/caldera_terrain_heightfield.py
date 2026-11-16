#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_terrain_heightfield: dump Caldera terrain sections as heightmaps.

Each ``superterrrain/**/*.terrain.usd`` composes to a single large ``height``
mesh: a REGULAR grid (X/Y horizontal, Z = height, 32-inch spacing) with
hundreds of thousands of quads. Importing it through the mesh pipeline is
wrong AND slow (the generic meshlet/LOD bake is superlinear and pointless for
a heightfield). This tool samples that grid directly and writes it as a
16-bit grayscale heightmap texture plus a small meta JSON — the heightfield
source that the engine's terrain pipeline (``PakAssetKind::Terrain`` /
``TerrainHField``) will consume, without ever building a triangle mesh.

Outputs (per section, into ``--out-dir``):
  ``<stem>.height.png``   16-bit grayscale, row-major, X fast / Y slow.
  ``<stem>.height.json``  origin + spacing + grid size + min/max height +
                          metersPerUnit/upAxis (to reconstruct world heights).

The height is normalized to [0, 65535] over the section's own min..max, so
``worldH = minH + (v / 65535) * (maxH - minH)`` in the USD stage's units
(inches here; multiply by metersPerUnit for meters). Missing grid cells
(holes where the island is not covered) are filled with the section minimum
and reported in the meta.

Example:
  python tools/caldera_terrain_heightfield.py \
      --usd-root G:/caldera_test/openusd_25.08 \
      --caldera-root G:/caldera_test/caldera \
      --out-dir G:/caldera_test/terrain_heightmaps
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import zlib
from pathlib import Path


def usd_env(usd_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    paths = [str(usd_root / "lib"), str(usd_root / "bin"),
             str(usd_root / "python"), str(usd_root / "plugin" / "usd")]
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    env["PXR_PLUGINPATH_NAME"] = str(usd_root / "lib" / "usd")
    env["PYTHONPATH"] = str(usd_root / "lib" / "python")
    return env


def discover_terrain(caldera_root: Path) -> list[Path]:
    island = caldera_root / "map_source" / "prefabs" / "br" / "wz_vg" / "mp_wz_island"
    return sorted((island / "superterrrain").rglob("*.terrain.usd"))


def _png_chunk(typ: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + typ + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


def write_png16(path: Path, w: int, h: int, pixels: list[int]) -> None:
    # 16-bit grayscale, filter 0 per row, big-endian samples.
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = pixels[y * w:(y + 1) * w]
        raw += struct.pack(">%dH" % w, *row)
    ihdr = struct.pack(">IIBBBBB", w, h, 16, 0, 0, 0, 0)  # bitdepth=16, gray
    png = (b"\x89PNG\r\n\x1a\n"
           + _png_chunk(b"IHDR", ihdr)
           + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + _png_chunk(b"IEND", b""))
    path.write_bytes(png)


def extract_heightfield(src: Path, out_dir: Path, env: dict[str, str],
                        usd_python: Path) -> dict | None:
    # Run in the bundled USD python: import pxr at runtime (keeps the host
    # interpreter USD-free).
    code = r"""
import json, sys, math
from pxr import Usd, UsdGeom
src, out_png, out_json = sys.argv[1], sys.argv[2], sys.argv[3]
stage = Usd.Stage.Open(src)
if not stage:
    sys.exit("cannot open stage: " + src)
# The terrain is the single big `height` mesh (regular X/Y grid, Z = height).
best = None
for p in stage.Traverse():
    if p.IsA(UsdGeom.Mesh) and p.GetName() == "height":
        m = UsdGeom.Mesh(p)
        pts = m.GetPointsAttr().Get()
        if pts is not None and len(pts) > 0:
            best = pts
            break
if best is None:
    sys.exit("no `height` mesh in " + src)
xs = [q[0] for q in best]; ys = [q[1] for q in best]; zs = [q[2] for q in best]
# Grid axes (regular spacing); guard against float noise with a tolerance.
ux = sorted(set(round(v, 6) for v in xs)); uy = sorted(set(round(v, 6) for v in ys))
if len(ux) < 2 or len(uy) < 2:
    sys.exit("height mesh is not a 2D grid")
spx = ux[1] - ux[0]; spy = uy[1] - uy[0]
minx, miny = ux[0], uy[0]
nx, ny = len(ux), len(uy)
minz, maxz = min(zs), max(zs)
span = (maxz - minz) if (maxz - minz) > 1e-9 else 1.0
# Rasterize: ix = (x - minx)/spx, iy = (y - miny)/spy.
grid = [None] * (nx * ny)
for q in best:
    ix = int(round((q[0] - minx) / spx)); iy = int(round((q[1] - miny) / spy))
    if 0 <= ix < nx and 0 <= iy < ny:
        grid[iy * nx + ix] = q[2]
holes = sum(1 for v in grid if v is None)
# Fill holes with the section minimum (water/edge -> lowest terrain).
pixels = []
for v in grid:
    z = minz if v is None else v
    pixels.append(max(0, min(65535, int(round((z - minz) / span * 65535.0)))))
import struct, zlib
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
raw = bytearray()
for y in range(ny):
    raw.append(0)
    raw += struct.pack(">%dH" % nx, *pixels[y * nx:(y + 1) * nx])
ihdr = struct.pack(">IIBBBBB", nx, ny, 16, 0, 0, 0, 0)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
       + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
open(out_png, "wb").write(png)
meta = {
    "origin": [minx, miny],
    "spacing": [spx, spy],
    "grid": [nx, ny],
    "height": [minz, maxz],
    "metersPerUnit": UsdGeom.GetStageMetersPerUnit(stage),
    "upAxis": str(UsdGeom.GetStageUpAxis(stage)),
    "holes": holes,
    "packing": "rg16",
}
json.dump(meta, open(out_json, "w"), indent=2, sort_keys=True)
print("  %s: grid %dx%d (spacing %.3f) height %.3f..%.3f holes=%d" % (
    src, nx, ny, spx, minz, maxz, holes))
"""
    usd_py = usd_python
    out_png = out_dir / (src.stem + ".height.png")
    out_json = out_dir / (src.stem + ".height.json")
    proc = __import__("subprocess").run(
        [str(usd_py), "-c", code, str(src), str(out_png), str(out_json)],
        env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        return {"path": str(src), "error": (proc.stderr or "exit").strip()}
    line = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    return {"path": str(src), "ok": True, "summary": line}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_terrain_heightfield")
    ap.add_argument("--usd-root", required=True, help="extracted OpenUSD prebuilt root")
    ap.add_argument("--caldera-root", required=True, help="Caldera repo root")
    ap.add_argument("--out-dir", required=True, help="heightmap output directory")
    args = ap.parse_args(argv)

    usd_root = Path(args.usd_root)
    caldera_root = Path(args.caldera_root)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    env = usd_env(usd_root)
    usd_python = usd_root / "python" / "python.exe"
    if not usd_python.exists():
        ap.error(f"USD python not found: {usd_python}")

    terrain = discover_terrain(caldera_root)
    print("caldera_terrain_heightfield: %d terrain sections" % len(terrain), flush=True)
    failures = 0
    for i, src in enumerate(terrain, 1):
        r = extract_heightfield(src, out_dir, env, usd_python)
        if r and r.get("ok"):
            print("  [%d/%d] %s" % (i, len(terrain), r["summary"]), flush=True)
        else:
            failures += 1
            print("  [%d/%d] %s FAILED: %s" % (
                i, len(terrain), src.name, (r or {}).get("error", "unknown")), flush=True)
    print("caldera_terrain_heightfield: %d failures -> %s" % (failures, out_dir), flush=True)
    return 0 if failures == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
