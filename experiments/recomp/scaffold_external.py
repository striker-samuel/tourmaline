#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scaffold/build a user-owned Tourmaline recomp asset project OUTSIDE git.

This invokes Tourmaline's real Project.exe pipeline (import -> ids ->
cook -> pak -> .tapp), not a bespoke pack format. The tiny bootstrap
triangle is generated outside git solely because v1 recipes require
a scene entry. Neither ROM nor extracted game content is copied.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
# The pipeline's actual extension grammar, not a wildcard catchall.
KINDS = frozenset({
    ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".ktx2",
    ".wav", ".spv", ".audiograph", ".spriteset",
    ".givolumemanifest",
})
BOOTSTRAP = "recomp_bootstrap"
SCENE_REL = "content/scenes/recomp_bootstrap.tour"
RECIPE = "recomp_assets.tourrecipe"


def external(path: Path, label: str) -> Path:
    if not path:
        raise ValueError(f"{label} is required")
    resolved = path.expanduser().resolve(strict=False)
    if resolved == REPO or REPO in resolved.parents:
        raise ValueError(f"{label} must be OUTSIDE the Tourmaline repository: {resolved}")
    return resolved


def invoke(exe: Path, *args: str) -> None:
    print("+", exe, *args, flush=True)
    subprocess.run([str(exe), *args], check=True)


def bootstrap_gltf(path: Path) -> None:
    """Portable one-triangle glTF with embedded non-proprietary geometry."""
    positions = [-1., -1., 0., 1., -1., 0., 0., 1., 0.]
    normals = [0., 0., 1.] * 3
    uvs = [0., 0., 1., 0., .5, 1.]
    blob = struct.pack("<9f9f6f3H", *positions, *normals, *uvs, 0, 1, 2)
    view_specs = [(0, 36, 34962), (36, 36, 34962),
                  (72, 24, 34962), (96, 6, 34963)]
    model = {
        "asset": {"version": "2.0", "generator": "Tourmaline recomp bootstrap"},
        "buffers": [{"byteLength": len(blob), "uri":
                     "data:application/octet-stream;base64," +
                     base64.b64encode(blob).decode("ascii")}],
        "bufferViews": [{"buffer": 0, "byteOffset": o,
                         "byteLength": n, "target": target}
                        for o, n, target in view_specs],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3,
             "type": "VEC3", "min": [-1., -1., 0.],
             "max": [1., 1., 0.]},
            {"bufferView": 1, "componentType": 5126, "count": 3,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3,
             "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 3,
             "type": "SCALAR"},
        ],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "mode": 4}]}],
        "nodes": [{"mesh": 0, "name": "RecompBootstrap"}],
        "scenes": [{"nodes": [0]}], "scene": 0,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(model, sort_keys=True) + "\n", encoding="utf-8")


def write_recipe(root: Path) -> list[str]:
    sources = root / "content/extracted"
    sources.mkdir(parents=True, exist_ok=True)
    assets = []
    for item in sorted(sources.rglob("*")):
        if not item.is_file():
            continue
        if item.is_symlink():
            raise ValueError(f"Refusing symlinked asset: {item}")
        if item.suffix.lower() not in KINDS:
            raise ValueError(
                f"Unsupported extracted asset {item.relative_to(root)}; "
                "convert it to a Tourmaline-supported image/audio/asset type")
        rel = item.relative_to(root).as_posix()
        if '"' in rel or "#" in rel or "\n" in rel:
            raise ValueError(f"Unquotable asset path: {rel}")
        assets.append(rel)
    lines = [
        "# Generated recipe, stored in the USER'S external project",
        "recipe 1", "project RecompAssets",
        f"entry {SCENE_REL}", "bake gi off", "bake cubemaps off",
        "", "[pak bootstrap]", "output recomp_bootstrap.pak",
        f"scene {SCENE_REL}",
    ]
    if assets:
        lines += ["depends extracted", "", "[pak extracted]",
                  "output recomp_extracted.pak"]
        for rel in assets:
            lines.append(f'include "{rel}"')
    (root / RECIPE).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return assets


def make_project(root: Path, exe: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    invoke(exe, "init", "--project", str(root), "--name", "RecompAssets")
    gltf = root / ".tourmaline" / "scaffold" / f"{BOOTSTRAP}.gltf"
    bootstrap_gltf(gltf)
    invoke(exe, "import", "--project", str(root),
           "--source", str(gltf), "--no-lods", "--no-meshlets")
    scene = root / SCENE_REL
    scene.parent.mkdir(parents=True, exist_ok=True)
    if not scene.exists():
        scene.write_text(
            '// Non-proprietary bootstrap scene for asset-only content.\n'
            'reference -root -n 0 -source '
            '"content/models/recomp_bootstrap/recomp_bootstrap.geometry" '
            '-name "ROOT";\n'
            'fileInfo -k "application" -v "tourmaline-tour";\n'
            'fileInfo -k "product" -v "Tourmaline";\n'
            'fileInfo -k "version" -v "1";\n',
            encoding="utf-8")
    assets = write_recipe(root)
    print(f"External content project: {root}")
    print(f"Extracted asset directory: {root / 'content/extracted'}")
    print(f"Recipe: {root / RECIPE} ({len(assets)} supported assets)")


def build(root: Path, publish: Path, exe: Path) -> None:
    if not (root / "project.tourproj").is_file():
        raise ValueError("No external Tourmaline project: run bootstrap first")
    if not (root / SCENE_REL).is_file():
        raise ValueError("Bootstrap scene missing: run bootstrap first")
    assets = write_recipe(root)
    publish.mkdir(parents=True, exist_ok=True)
    generated = root / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    header = generated / "project.h"
    recipe = root / RECIPE
    invoke(exe, "validate", "--project", str(root), "--recipe", str(recipe))
    invoke(exe, "ids", "--project", str(root), "--recipe", str(recipe),
           "--header", str(header), "--pak-dir", str(publish))
    invoke(exe, "build", "--project", str(root), "--recipe", str(recipe),
           "--publish", str(publish), "--manifest-name", "recomp_assets",
           "--header", str(header))
    print(f"Published {len(assets)} assets through Tourmaline: {publish}")
    print(f"Generated stable catalog (outside git): {header}")
    print(f"Use --assets {publish / 'recomp_assets.tapp'} with RecompDiagnostic")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("bootstrap", "build"))
    parser.add_argument("--root", type=Path, required=True,
                        help="External project directory (content and .tourmaline artifacts)")
    parser.add_argument("--project-tool", type=Path, required=True,
                        help="Compiled Project[.exe] from Tourmaline")
    parser.add_argument("--publish", type=Path,
                        help="External cooked .pak/.tapp output (required for build)")
    parser.add_argument("--rom", type=Path,
                        help="Optional user-owned ROM path; only checks location, never copies")
    parser.add_argument("--artifacts", type=Path,
                        help="Optional external recomp-generated code/build directory")
    args = parser.parse_args()
    try:
        root = external(args.root, "Project root")
        exe = args.project_tool.expanduser().resolve(strict=True)
        if not exe.is_file():
            raise ValueError(f"Project tool not found: {exe}")
        if args.rom:
            external(args.rom, "ROM")
        if args.artifacts:
            external(args.artifacts, "Recompiled artifacts")
        publish = external(args.publish, "Publish output") if args.publish else None
        if args.command == "bootstrap":
            make_project(root, exe)
        else:
            if publish is None:
                raise ValueError("--publish is required for build")
            build(root, publish, exe)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f"recomp assets: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
