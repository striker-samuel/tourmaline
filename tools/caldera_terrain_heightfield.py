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
    # The terrain SECTION ASSEMBLY is st_<x>.usd; it references the
    # st_<x>.terrain.usd mesh and places the `height` grid in world space via
    # a parent xform (super_terrain_<n>). Extracting from the .terrain.usd
    # mesh directly reads identity-space points, so every section overlaps at
    # the origin. We therefore open the assembly and apply the height mesh's
    # composed world transform below.
    island = caldera_root / "map_source" / "prefabs" / "br" / "wz_vg" / "mp_wz_island"
    files: list[Path] = []
    for season in ("season_4", "season_5"):
        for p in sorted((island / "superterrrain" / season).glob("st_*.usd")):
            if ".terrain" in p.name or ".splined_models" in p.name:
                continue
            files.append(p)
    return files


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
from pxr import Usd, UsdGeom, Gf
src, out_png, out_json = sys.argv[1], sys.argv[2], sys.argv[3]
stage = Usd.Stage.Open(src)
if not stage:
    sys.exit("cannot open stage: " + src)
# The terrain is the single big `height` mesh (regular X/Y grid, Z = height).
# The assembly's parent xform places it in world space; the mesh's own
# points are identity-space. Transform to world so sections land at their
# true island offsets (a pure translation per section here, but apply the
# full matrix for generality).
best = None
for p in stage.Traverse():
    if p.IsA(UsdGeom.Mesh) and p.GetName() == "height":
        m = UsdGeom.Mesh(p)
        pts = m.GetPointsAttr().Get()
        if pts is not None and len(pts) > 0:
            world = m.ComputeLocalToWorldTransform(Usd.TimeCode.Default())
            best = [world.Transform(Gf.Vec3d(float(q[0]), float(q[1]), float(q[2])))
                    for q in pts]
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


def stitch_heightfields(sources: list[Path], out_dir: Path, env: dict[str, str],
                        usd_python: Path) -> dict | None:
    # One USD subprocess stitches every section into ONE heightfield: extract
    # each `height` grid (holes kept as None), place them into the union grid
    # at their world offset, and emit a single heightmap + meta. All sections
    # share the 32-inch spacing, so the union is one regular grid.
    code = r"""
import json, sys
from pxr import Usd, UsdGeom, Gf
srcs, out_png, out_json = json.loads(sys.argv[1]), sys.argv[2], sys.argv[3]

def extract_grid(path):
    stage = Usd.Stage.Open(path)
    for p in stage.Traverse():
        if p.IsA(UsdGeom.Mesh) and p.GetName() == "height":
            m = UsdGeom.Mesh(p)
            pts = m.GetPointsAttr().Get()
            if not pts or len(pts) == 0:
                return None
            world = m.ComputeLocalToWorldTransform(Usd.TimeCode.Default())
            pts = [world.Transform(Gf.Vec3d(float(q[0]), float(q[1]), float(q[2])))
                   for q in pts]
            xs = [q[0] for q in pts]; ys = [q[1] for q in pts]; zs = [q[2] for q in pts]
            ux = sorted(set(round(v, 6) for v in xs)); uy = sorted(set(round(v, 6) for v in ys))
            if len(ux) < 2 or len(uy) < 2:
                return None
            spx, spy = ux[1] - ux[0], uy[1] - uy[0]
            minx, miny = ux[0], uy[0]
            nx, ny = len(ux), len(uy)
            grid = [None] * (nx * ny)
            for q in pts:
                ix = int(round((q[0] - minx) / spx)); iy = int(round((q[1] - miny) / spy))
                if 0 <= ix < nx and 0 <= iy < ny:
                    grid[iy * nx + ix] = q[2]
            return (minx, miny, spx, spy, nx, ny, grid)
    return None

secs = [g for g in (extract_grid(s) for s in srcs) if g]
if not secs:
    sys.exit("no height grids extracted")
spx, spy = secs[0][2], secs[0][3]
minx = min(g[0] for g in secs); miny = min(g[1] for g in secs)
maxx = max(g[0] + (g[4] - 1) * g[2] for g in secs)
maxy = max(g[1] + (g[5] - 1) * g[3] for g in secs)
unx = int(round((maxx - minx) / spx)) + 1
uny = int(round((maxy - miny) / spy)) + 1
# Stitch (later sections overwrite earlier overlaps; uncovered cells stay None).
union = [None] * (unx * uny)
for g in secs:
    ox = int(round((g[0] - minx) / spx)); oy = int(round((g[1] - miny) / spy))
    for iy in range(g[5]):
        for ix in range(g[4]):
            h = g[6][iy * g[4] + ix]
            if h is not None:
                union[(oy + iy) * unx + (ox + ix)] = h
zs = [h for h in union if h is not None]
minz, maxz = min(zs), max(zs)
span = (maxz - minz) if (maxz - minz) > 1e-9 else 1.0
holes = sum(1 for h in union if h is None)
# Discontinuity analysis: adjacent-cell height jumps (the sections overlap
# with conflicting heights, so a naive last-wins stitch can leave seams where
# the winning section changes). Report the largest jump + how many cell edges
# exceed 2x / 10x the grid spacing.
max_jump = 0.0
steep_edges = 0
seam_edges = 0
for iy in range(uny):
    for ix in range(unx):
        h = union[iy * unx + ix]
        if h is None:
            continue
        for dx, dy in ((1, 0), (0, 1)):
            nx, ny = ix + dx, iy + dy
            if nx >= unx or ny >= uny:
                continue
            n = union[ny * unx + nx]
            if n is None:
                continue
            d = abs(h - n)
            max_jump = max(max_jump, d)
            if d > 2.0 * spx:
                steep_edges += 1
            if d > 10.0 * spx:
                seam_edges += 1
pixels = []
for h in union:
    z = minz if h is None else h
    pixels.append(max(0, min(65535, int(round((z - minz) / span * 65535.0)))))
import struct, zlib
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
raw = bytearray()
for y in range(uny):
    raw.append(0)
    raw += struct.pack(">%dH" % unx, *pixels[y * unx:(y + 1) * unx])
ihdr = struct.pack(">IIBBBBB", unx, uny, 16, 0, 0, 0, 0)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
       + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
open(out_png, "wb").write(png)
stage = Usd.Stage.Open(srcs[0])
meta = {
    "origin": [minx, miny],
    "spacing": [spx, spy],
    "grid": [unx, uny],
    "height": [minz, maxz],
    "metersPerUnit": UsdGeom.GetStageMetersPerUnit(stage),
    "upAxis": str(UsdGeom.GetStageUpAxis(stage)),
    "holes": holes,
    "sections": len(secs),
    "maxJump": max_jump,
    "steepEdges": steep_edges,
    "seamEdges": seam_edges,
    "packing": "rg16",
}
json.dump(meta, open(out_json, "w"), indent=2, sort_keys=True)
print("stitched %d sections -> grid %dx%d height %.3f..%.3f holes=%d maxJump=%.1f steepEdges=%d seamEdges=%d" % (
    len(secs), unx, uny, minz, maxz, holes, max_jump, steep_edges, seam_edges))
"""
    out_png = out_dir / "island.height.png"
    out_json = out_dir / "island.height.json"
    proc = __import__("subprocess").run(
        [str(usd_python), "-c", code, json.dumps([str(s) for s in sources]),
         str(out_png), str(out_json)],
        env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        return {"error": (proc.stderr or "exit").strip()}
    line = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    return {"ok": True, "summary": line}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_terrain_heightfield")
    ap.add_argument("--usd-root", required=True, help="extracted OpenUSD prebuilt root")
    ap.add_argument("--caldera-root", required=True, help="Caldera repo root")
    ap.add_argument("--out-dir", required=True, help="heightmap output directory")
    ap.add_argument("--stitch", action="store_true",
                    help="stitch all sections into ONE island heightmap instead of per-section files")
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
    if args.stitch:
        r = stitch_heightfields(terrain, out_dir, env, usd_python)
        if r and r.get("ok"):
            print("  " + r["summary"], flush=True)
            return 0
        print("  stitch FAILED: %s" % (r or {}).get("error", "unknown"), flush=True)
        return 2
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
