#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_island_scaffold: assemble the imported Caldera districts into one project.

The batch import (caldera_batch_import.py) already derived every district's
native artifact family (`.geometry` + `.material` + `.usd.meta`) next to its
`.usd` source. This tool stages those artifacts into a Tourmaline PROJECT and
authors the single island scene + recipe that cook into the full-island pak:

  <project>/content/models/<stem>/<stem>.geometry/.material/.usd.meta
  <project>/content/scenes/island.tour        (one reference per district)
  <project>/island.tourrecipe                  (one [pak world])

The island scene references every district's geometry under the scene root at
identity transform — each district's meshes are already in the authored Caldera
world space (the USD adapter's basis node carries the coordinate conversion),
so referencing them side by side places them exactly.

Example:
  python tools/caldera_island_scaffold.py \
      --caldera-root G:/caldera_test/caldera \
      --project-dir G:/caldera_test/island \
      --project-exe out/build/x64-Release/Project.exe
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

_ISLAND = "map_source/prefabs/br/wz_vg/mp_wz_island"


def discover_districts(caldera_root: Path) -> list[str]:
    season = caldera_root / _ISLAND / "season_4"
    stems: list[str] = []
    for p in sorted(season.glob("map_*.usd")):
        if ".splined_models" in p.name or "_audio" in p.name:
            continue
        stems.append(p.stem)
    return stems


def stage_district(caldera_root: Path, project_dir: Path, stem: str) -> None:
    season = caldera_root / _ISLAND / "season_4"
    dst = project_dir / "content" / "models" / stem
    dst.mkdir(parents=True, exist_ok=True)
    for suffix in (".geometry", ".mat0_default.material", ".usd.meta"):
        src = season / (stem + suffix)
        if not src.exists():
            print(f"  WARN: missing {src.name} for {stem}", flush=True)
            continue
        shutil.copy2(src, dst / src.name)
    # PARTITIONED-FAMILY parts (P1): a split district ships <stem>_partN.
    # geometry files beside the manifest. The authoring stays agnostic —
    # the .tour still references the single logical <stem>.geometry, and
    # the cook/runtime resolve the parts through the manifest.
    for part in sorted(season.glob(stem + "_part*.geometry")):
        shutil.copy2(part, dst / part.name)


def write_island_tour(project_dir: Path, stems: list[str]) -> Path:
    scenes = project_dir / "content" / "scenes"
    scenes.mkdir(parents=True, exist_ok=True)
    tour = scenes / "island.tour"
    lines = [
        "//Name: island.tour",
        f"reference -root -n 0 -source \"content/models/{stems[0]}/{stems[0]}.geometry\""
        ' -name "ROOT";',
    ]
    for stem in stems[1:]:
        lines.append(
            f'reference -n 0 -source "content/models/{stem}/{stem}.geometry" -name "{stem}";'
        )
    lines += [
        'fileInfo -k "application" -v "tourmaline-tour";',
        'fileInfo -k "product" -v "Tourmaline";',
        'fileInfo -k "version" -v "1";',
        'materialAsset -default -name "default";',
        "",
    ]
    tour.write_text("\n".join(lines), encoding="utf-8")
    return tour


def write_recipe(project_dir: Path) -> Path:
    recipe = project_dir / "island.tourrecipe"
    recipe.write_text(
        "# Tourmaline Project Recipe v1\n"
        "# Caldera full island (geometry-only, no GI bake).\n"
        "recipe 1\n"
        "project CalderaIsland\n"
        "entry content/scenes/island.tour\n"
        "\n"
        "bake gi off\n"
        "bake cubemaps off\n"
        "bake sdf off\n"
        "\n"
        "[pak world]\n"
        "output island.pak\n"
        "scene content/scenes/island.tour\n",
        encoding="utf-8",
    )
    return recipe


def generate_terrain(caldera_root: Path, project_dir: Path, usd_root: Path,
                     terrain_src: Path, heightmaps: Path, tools_dir: Path) -> bool:
    """Run the two terrain-source stages into the project:
      1. caldera_terrain_heightfield.py --stitch -> the island heightmap (+ JSON + coverage)
      2. caldera_terrain_tiles.py             -> terrain.meta + h_XX_ZZ.png + m_XX_ZZ.png
    Needs the OpenUSD prebuilt (--usd-root) and PIL in the host interpreter
    (caldera_terrain_tiles.py imports PIL). Returns True on success."""
    heightfield_py = tools_dir / "caldera_terrain_heightfield.py"
    tiles_py = tools_dir / "caldera_terrain_tiles.py"
    heightmaps.mkdir(parents=True, exist_ok=True)
    terrain_src.mkdir(parents=True, exist_ok=True)

    print("  [terrain] stitching island heightfield ...", flush=True)
    r = subprocess.run(
        [sys.executable, str(heightfield_py),
         "--usd-root", str(usd_root),
         "--caldera-root", str(caldera_root),
         "--out-dir", str(heightmaps),
         "--stitch"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        tail = "\n".join((r.stderr or "").splitlines()[-3:])
        print("  [terrain] heightfield FAILED:\n" + tail, flush=True)
        return False

    print("  [terrain] resampling heightmap -> terrain source tiles ...", flush=True)
    r = subprocess.run(
        [sys.executable, str(tiles_py),
         "--heightmap", str(heightmaps / "island.height.png"),
         "--meta", str(heightmaps / "island.height.json"),
         "--coverage", str(heightmaps / "island.coverage.png"),
         "--out", str(terrain_src)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        tail = "\n".join((r.stderr or "").splitlines()[-3:])
        print("  [terrain] tiles FAILED:\n" + tail, flush=True)
        return False
    print(f"  [terrain] wrote {terrain_src}", flush=True)
    return True


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_island_scaffold")
    ap.add_argument("--caldera-root", required=True, help="Caldera repo root")
    ap.add_argument("--project-dir", required=True, help="destination Tourmaline project dir")
    ap.add_argument("--project-exe", required=True, help="path to the built Project.exe")
    ap.add_argument("--with-terrain", action="store_true",
                    help="also generate the island terrain source (needs --usd-root + PIL)")
    ap.add_argument("--usd-root", help="extracted OpenUSD prebuilt root (for --with-terrain)")
    ap.add_argument("--terrain-src", help="terrain source output dir (default <project>/terrain_src)")
    ap.add_argument("--heightmaps", help="heightmap output dir (default <project>/terrain_heightmaps)")
    ap.add_argument("--pak-exe", help="optional Pak.exe; cook the terrain pak directly")
    args = ap.parse_args(argv)

    caldera_root = Path(args.caldera_root)
    project_dir = Path(args.project_dir)
    project_exe = Path(args.project_exe)
    if not project_exe.exists():
        ap.error(f"Project.exe not found: {project_exe}")
    if args.with_terrain and not args.usd_root:
        ap.error("--with-terrain requires --usd-root (the extracted OpenUSD prebuilt)")

    tools_dir = Path(__file__).resolve().parent

    stems = discover_districts(caldera_root)
    if not stems:
        ap.error("no map_*.usd districts found under " + str(caldera_root / _ISLAND / "season_4"))
    print(f"caldera_island_scaffold: {len(stems)} districts", flush=True)

    # Init the project marker (mints a persistent guid) when absent.
    marker = project_dir / "project.tourproj"
    if not marker.exists():
        project_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [str(project_exe), "init", "--project", str(project_dir), "--name", "CalderaIsland"],
            check=True,
        )
        print("  project initialized", flush=True)

    for i, stem in enumerate(stems, 1):
        stage_district(caldera_root, project_dir, stem)
        print(f"  [{i}/{len(stems)}] staged {stem}", flush=True)

    tour = write_island_tour(project_dir, stems)
    recipe = write_recipe(project_dir)
    print(f"  wrote {tour.relative_to(project_dir)}", flush=True)
    print(f"  wrote {recipe.relative_to(project_dir)}", flush=True)

    terrain_src = Path(args.terrain_src) if args.terrain_src else project_dir / "terrain_src"
    if args.with_terrain:
        heightmaps = Path(args.heightmaps) if args.heightmaps else project_dir / "terrain_heightmaps"
        if not generate_terrain(caldera_root, project_dir, Path(args.usd_root),
                                terrain_src, heightmaps, tools_dir):
            return 2

    print("scaffold done; next: "
          f"Project.exe validate --project {project_dir} --recipe {recipe}", flush=True)
    if args.with_terrain:
        # Terrain is a SEPARATE pak (the mesh pak stays untouched): the loader
        # harvests Terrain blobs from any mounted pak (PakWorldLoader.cpp). Cook
        # it into <project>/build/island_terrain.pak (beside the built pak) and
        # list it in the built build/island.tapp [paks] section.
        terrain_pak = project_dir / "build" / "island_terrain.pak"
        pak_exe = Path(args.pak_exe) if args.pak_exe else None
        print("terrain content (separate pak, run after Project build):", flush=True)
        if pak_exe:
            if not pak_exe.exists():
                print(f"  WARN: Pak.exe not found at {pak_exe}; cook manually", flush=True)
            else:
                terrain_pak.parent.mkdir(parents=True, exist_ok=True)
                r = subprocess.run(
                    [str(pak_exe), str(project_dir / "content"),
                     "--terrain", str(terrain_src),
                     "--out-pak", str(terrain_pak)],
                    capture_output=True, text=True, encoding="utf-8", errors="replace")
                if r.returncode != 0:
                    tail = "\n".join((r.stderr or "").splitlines()[-3:])
                    print("  [terrain] pak cook FAILED:\n" + tail, flush=True)
                    return 2
                print(f"  cooked {terrain_pak}", flush=True)
        else:
            print(f"  Pak.exe {project_dir / 'content'} --terrain {terrain_src} "
                  f"--out-pak {terrain_pak}", flush=True)
        print("  then register it in the built manifest (after Project build):", flush=True)
        print(f"    python {tools_dir / 'caldera_add_terrain.py'} "
              f"--project-dir {project_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
