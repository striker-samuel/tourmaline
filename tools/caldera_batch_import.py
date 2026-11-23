#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_batch_import: convert the whole Caldera island district-by-district.

Runs the built `Import.exe` on every district assembly (`season_4/map_*.usd`),
capturing per-file geometry/instance/material stats, and writes a JSON + text
summary. This is the M4 "complete island converted" step: coverage is validated
against the audit's content model. Cross-district shared-mesh dedup (Phase 3)
and `.splined_models` splines are separate concerns and are reported, not
silently dropped. Terrain sections (`superterrrain/**/*.terrain.usd`) are
NOT meshes — they are heightfield grids, extracted by
caldera_terrain_heightfield.py into 16-bit heightmap textures.

The USD runtime DLLs (pxr lib/bin/python/plugin) must be resolvable; pass
`--usd-root` pointing at the extracted OpenUSD prebuilt.

Examples:
  python tools/caldera_batch_import.py \
      --import-exe out/build/x64-Release/Import.exe \
      --usd-root G:/caldera_test/openusd_25.08 \
      --caldera-root G:/caldera_test/caldera \
      --out G:/caldera_test/batch_import.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_RE_STATS = re.compile(
    r"materials=(\d+) anims=(\d+) geometryBlobs=(\d+) instances=(\d+)"
    r" lights=(\d+) cameras=(\d+) nodes=(\d+)"
)


def usd_env(usd_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    paths = [str(usd_root / "lib"), str(usd_root / "bin"),
             str(usd_root / "python"), str(usd_root / "plugin" / "usd")]
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    # USD resolves its asset resolver + file-format plugins through plugInfo
    # registries; the prebuilt's top-level registry lives under lib/usd/. Point
    # PXR_PLUGINPATH_NAME there so ArDefaultResolver and the .usd/.usdc plugins
    # are found regardless of where the USD DLLs were loaded from.
    env["PXR_PLUGINPATH_NAME"] = str(usd_root / "lib" / "usd")
    return env


def discover_districts(caldera_root: Path) -> list[Path]:
    island = caldera_root / "map_source" / "prefabs" / "br" / "wz_vg" / "mp_wz_island"
    files: list[Path] = []
    # District assemblies: the spatial regions authored under season_4/.
    for p in sorted((island / "season_4").glob("map_*.usd")):
        # The .splined_models twins are spline assemblies (not meshes); skip
        # them here and report the spline concern separately.
        if ".splined_models" in p.name or "_audio" in p.name:
            continue
        files.append(p)
    # Terrain sections (superterrrain/**/*.terrain.usd) are NOT imported as
    # meshes: they are heightfield grids, extracted separately by
    # caldera_terrain_heightfield.py -> 16-bit heightmap textures + meta.
    return files


def import_one(import_exe: Path, env: dict[str, str], src: Path) -> dict:
    start = time.time()
    proc = subprocess.run(
        [str(import_exe), str(src), "--force", "--lods", "off"],
        env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    elapsed = time.time() - start
    result = {
        "path": str(src),
        "exit": proc.returncode,
        "elapsed_s": round(elapsed, 1),
        "error": None,
    }
    m = _RE_STATS.search(proc.stdout + proc.stderr)
    if m:
        result.update({
            "materials": int(m.group(1)), "anims": int(m.group(2)),
            "geometry_blobs": int(m.group(3)), "instances": int(m.group(4)),
            "lights": int(m.group(5)), "cameras": int(m.group(6)),
            "nodes": int(m.group(7)),
        })
    if proc.returncode != 0:
        # Capture the last meaningful line of stderr as the failure reason.
        lines = [l for l in (proc.stderr or "").splitlines() if l.strip()]
        result["error"] = lines[-1].strip() if lines else f"exit {proc.returncode}"
    return result


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_batch_import")
    ap.add_argument("--import-exe", required=True, help="path to the built Import.exe")
    ap.add_argument("--usd-root", required=True, help="extracted OpenUSD prebuilt root")
    ap.add_argument("--caldera-root", required=True, help="Caldera repo root")
    ap.add_argument("--out", required=True, help="JSON output path")
    ap.add_argument("--limit", type=int, default=0, help="import at most N files (0 = all)")
    args = ap.parse_args(argv)

    import_exe = Path(args.import_exe)
    usd_root = Path(args.usd_root)
    caldera_root = Path(args.caldera_root)
    if not import_exe.exists():
        ap.error(f"Import.exe not found: {import_exe}")
    env = usd_env(usd_root)

    districts = discover_districts(caldera_root)
    if args.limit > 0:
        districts = districts[: args.limit]

    results = []
    total_blobs = total_instances = 0
    failures = 0
    t0 = time.time()
    print(f"caldera_batch_import: {len(districts)} files", flush=True)
    for i, src in enumerate(districts, 1):
        r = import_one(import_exe, env, src)
        results.append(r)
        if r["exit"] == 0:
            total_blobs += r.get("geometry_blobs", 0)
            total_instances += r.get("instances", 0)
            status = (f"blobs={r.get('geometry_blobs', '?')} "
                      f"inst={r.get('instances', '?')} {r['elapsed_s']}s")
        else:
            failures += 1
            status = f"FAILED: {r['error']}"
        print(f"  [{i}/{len(districts)}] {src.name}: {status}", flush=True)

    elapsed = time.time() - t0
    summary = {
        "files": len(results),
        "failures": failures,
        "total_geometry_blobs": total_blobs,
        "total_instances": total_instances,
        "elapsed_s": round(elapsed, 1),
        "results": results,
    }
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    print(f"caldera_batch_import: {failures} failures, "
          f"{total_blobs} blobs, {total_instances} instances, "
          f"{round(elapsed, 1)}s -> {args.out}", flush=True)
    return 0 if failures == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
