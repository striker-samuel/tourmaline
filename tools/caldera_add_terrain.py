#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""caldera_add_terrain: append the island terrain pak to a built .tapp manifest.

The island terrain ships as a SEPARATE pak (cooked by
``caldera_island_scaffold.py --with-terrain`` / ``Pak.exe --terrain``). After
``Project.exe build`` writes ``build/island.tapp``, this tool adds
``build/island_terrain.pak`` to the manifest's ``[paks]`` section so the loader
harvests its Terrain blobs (PakWorldLoader.cpp: "Terrain rides ANY session pak").

Idempotent: a manifest already listing the terrain pak is left unchanged.

Usage:
  python tools/caldera_add_terrain.py --project-dir G:/caldera_test/island_fixed
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def add_pak_to_manifest(manifest: Path, pak_name: str) -> tuple[bool, str]:
    """Append `pak_name` to the manifest's [paks] section. Returns (changed, msg)."""
    if not manifest.exists():
        return False, f"missing manifest {manifest}"
    text = manifest.read_text(encoding="utf-8")
    lines = text.splitlines()

    paks_idx = None
    for i, line in enumerate(lines):
        if line.strip() == "[paks]":
            paks_idx = i
            break
    if paks_idx is None:
        return False, f"no [paks] section in {manifest}"

    # The section runs from paks_idx+1 until the next [section] header or EOF.
    section_end = len(lines)
    for i in range(paks_idx + 1, len(lines)):
        s = lines[i].strip()
        if s.startswith("[") and s.endswith("]") and s != "[paks]":
            section_end = i
            break
    section = lines[paks_idx + 1 : section_end]

    if any(line.strip() == pak_name for line in section):
        return False, f"already listed: {pak_name}"

    # Append after the last non-empty entry (or directly under [paks]).
    insert_idx = paks_idx + 1
    for i in range(paks_idx + 1, section_end):
        if lines[i].strip():
            insert_idx = i + 1
    lines.insert(insert_idx, pak_name)
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return True, f"added {pak_name} to {manifest}"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="caldera_add_terrain")
    ap.add_argument("--project-dir", required=True, help="Tourmaline project dir")
    ap.add_argument("--manifest", help="manifest path (default <project>/build/island.tapp)")
    ap.add_argument("--pak-name", default="island_terrain.pak", help="terrain pak file name")
    args = ap.parse_args(argv)

    project_dir = Path(args.project_dir)
    manifest = Path(args.manifest) if args.manifest else project_dir / "build" / "island.tapp"
    pak_name = args.pak_name

    terrain_pak = manifest.parent / pak_name
    if not terrain_pak.exists():
        print(f"WARN: {terrain_pak} does not exist - cook it first "
              f"(Pak.exe <content> --terrain <terrain_src> --out-pak {terrain_pak})", flush=True)

    changed, msg = add_pak_to_manifest(manifest, pak_name)
    print(msg, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
