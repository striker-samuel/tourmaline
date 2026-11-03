#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Scaffold a Tourmaline project using the canonical native Project init.

The native Project executable owns project.tourproj and its persistent GUID;
this helper never invents or rewrites a project identity. It creates a
version-1 .tourrecipe, and optionally a README and .gitignore.

  python tools/project_init.py --name "My Game" --path ~/games/my-game \
      --project-exe out/build/x64-Release/Project.exe --json
  python tools/project_init.py --name Sponza --path ./sponza \
      --from-gltf /assets/sponza.glb --from-scene /assets/sponza.tour \
      --pak world --bake-gi on --dry-run --json

A default recipe is a scaffold, not a cooked/buildable project. The entry
scene must exist and reference imported geometry before Project validate/build.
No overwrite or adoption without an explicit --adopt.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


class InitError(Exception):
    pass


def parser():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", required=True, help="project display name")
    p.add_argument("--path", required=True, type=Path, help="destination project root")
    p.add_argument("--project-exe", type=Path,
                   help="built native Project executable (autodetect on PATH/build dirs)")
    p.add_argument("--recipe-name", default="game", help="recipe basename (default game)")
    p.add_argument("--pak", default="world", help="pak ID and output basename (default world)")
    p.add_argument("--entry-scene", default="content/scenes/main.tour",
                   help="project-relative .tour scene path")
    p.add_argument("--from-gltf", type=Path, help="optional external .gltf/.glb to import")
    p.add_argument("--from-scene", type=Path,
                   help="optional existing .tour to seed (requires --from-gltf)")
    p.add_argument("--bake-gi", choices=("on", "off"), default="off")
    p.add_argument("--bake-cubemaps", choices=("on", "off"), default="off")
    p.add_argument("--no-recipe", action="store_true", help="only initialize project root")
    p.add_argument("--readme", action="store_true", help="also generate quick-start README.md")
    p.add_argument("--gitignore", action="store_true", help="also seed a project .gitignore")
    p.add_argument("--adopt", action="store_true",
                   help="allow existing project marker; never replace its GUID or files")
    p.add_argument("--dry-run", action="store_true", help="preview only, no filesystem changes")
    p.add_argument("--json", action="store_true", help="structured output")
    return p


def _relative(raw, ext=None):
    path = str(raw).replace("\\", "/")
    if (not path or path.startswith("/") or re.match(r"^[A-Za-z]:", path)
            or any(part in ("", ".", "..") for part in path.split("/"))
            or '"' in path or "#" in path or "\n" in path or "\r" in path):
        raise InitError(f"invalid project-relative path: {raw!r}")
    if ext and not path.lower().endswith(ext):
        raise InitError(f"path must end with {ext}: {raw!r}")
    if any(ch in path for ch in "*?"):
        raise InitError(f"wildcards not allowed in project-relative path: {raw!r}")
    return path


def _label(label, what):
    if (not label or not label.strip() or len(label) > 120
            or any(ch in label for ch in '\r\n"\\#[]')):
        raise InitError(f"{what} must be 1..120 characters without quotes, newline or reserved characters")
    return label.strip()


def _basename(name, what):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", name):
        raise InitError(f"{what} must match [A-Za-z0-9][A-Za-z0-9_-]{{0,63}}")
    return name


def _native_path(args):
    if args.project_exe is not None:
        return args.project_exe.expanduser().resolve()
    for name in ("Project", "Project.exe"):
        found = shutil.which(name)
        if found:
            return Path(found).resolve()
    root = Path(__file__).resolve().parents[1]
    for rel in ("out/build/x64-Release/Project.exe",
                "out/build/linux-Release/Project",
                "out/build/x64-Debug/Project.exe"):
        path = root / rel
        if path.is_file():
            return path.resolve()
    return None


def _existing_marker(root, name, adopt):
    marker = root / "project.tourproj"
    if not root.exists():
        return None
    if not root.is_dir():
        raise InitError(f"destination exists and is not a directory: {root}")
    if not marker.is_file():
        if any(root.iterdir()):
            raise InitError(f"destination is nonempty and has no project marker: {root}")
        return None
    if not adopt:
        raise InitError(f"project already exists: {root}; use --adopt to keep its identity")
    content = marker.read_text(encoding="utf-8")
    metadata = dict(line.split("=", 1) for line in content.splitlines() if "=" in line)
    if metadata.get("name") != name:
        raise InitError(f"existing project's name is {metadata.get('name')!r}, not {name!r}")
    if not re.fullmatch(r"[0-9a-fA-F]{32}", metadata.get("guid", "")):
        raise InitError("existing project marker has invalid GUID; use native Project init to repair")
    return metadata["guid"]


def plan(args):
    name = _label(args.name, "project name")
    recipe_name = _basename(args.recipe_name, "recipe name")
    pak_name = _basename(args.pak, "pak ID")
    root = args.path.expanduser().resolve()
    if root == root.parent:
        raise InitError("refusing to initialize a filesystem root")
    if args.from_scene is not None and args.from_gltf is None:
        raise InitError("--from-scene requires --from-gltf to import its geometry")
    if args.from_gltf and args.from_gltf.suffix.lower() not in (".gltf", ".glb"):
        raise InitError("--from-gltf requires .gltf or .glb")
    if args.from_scene and args.from_scene.suffix.lower() != ".tour":
        raise InitError("--from-scene requires .tour")
    for src in (args.from_gltf, args.from_scene):
        if src is not None and not src.expanduser().is_file():
            raise InitError(f"source file not found: {src}")
    entry = _relative(args.entry_scene, ".tour")
    if args.from_scene is not None:
        seeded = "content/scenes/" + args.from_scene.name
        if entry != "content/scenes/main.tour" and entry != seeded:
            raise InitError(f"--entry-scene must be {seeded!r} when --from-scene is set")
        entry = seeded
    guid = _existing_marker(root, name, args.adopt)
    recipe = root / (recipe_name + ".tourrecipe")
    files = []
    if not args.no_recipe:
        files.append(recipe)
    if args.readme:
        files.append(root / "README.md")
    if args.gitignore:
        files.append(root / ".gitignore")
    for dest in files:
        if dest.exists():
            raise InitError(f"refusing to overwrite existing file: {dest}")
    native = _native_path(args)
    if not args.dry_run and (native is None or not native.is_file()):
        raise InitError("native Project executable not found; pass --project-exe or build Project")
    argv = [str(native or "Project"), "init", "--project", str(root), "--name", name]
    if args.from_gltf:
        argv += ["--from-gltf", str(args.from_gltf.expanduser().resolve())]
    if args.from_scene:
        argv += ["--from-scene", str(args.from_scene.expanduser().resolve())]
    return {"schema_version": 1, "project": name, "root": str(root),
            "recipe": str(recipe) if not args.no_recipe else None,
            "entry_scene": entry, "pak_id": pak_name, "pak_output": pak_name + ".pak",
            "bake_gi": args.bake_gi, "bake_cubemaps": args.bake_cubemaps,
            "existing_guid": guid, "native_command": argv,
            "files_to_create": [str(f) for f in files],
            "scene_ready": bool((root / entry).is_file() or args.from_scene),
            "status": "planned"}


def _scaffold(recipe, name, entry, pak, gi, cubemaps):
    return (f"# Tourmaline Project Recipe v1\n"
            f"# The entry .tour must reference real project content before building.\n"
            f"recipe 1\nproject {name}\nentry {entry}\n\n"
            f"bake gi {gi}\nbake cubemaps {cubemaps}\n\n"
            f"[pak {pak}]\noutput {pak}.pak\nscene {entry}\n")


def _readme(info):
    return (f"# {info['project']}\n\n"
            f"Tourmaline project created with `tools/project_init.py`.\n\n"
            f"- Scene: `{info['entry_scene']}`\n"
            f"- Recipe: `{Path(info['recipe']).name if info['recipe'] else '(not generated)'}`\n"
            f"- Cooked pak: `{info['pak_output']}`\n\n"
            "Import a model and author a scene if the entry file does not exist yet.\n"
            "A recipe scaffold is not buildable until its root scene references real geometry.\n\n"
            "Then validate and build with the native Project executable:\n\n"
            "```bash\n"
            f"Project validate --project . --recipe {Path(info['recipe']).name if info['recipe'] else 'game.tourrecipe'}\n"
            f"Project build --project . --recipe {Path(info['recipe']).name if info['recipe'] else 'game.tourrecipe'}\n"
            "```\n")


def _emit(info, structured):
    if structured:
        print(json.dumps(info, sort_keys=True))
    else:
        print(f"project_init: {info['status']}: {info['project']} at {info['root']}")
        print(f"  recipe: {info['recipe'] or '(disabled)'}")
        if not info["scene_ready"]:
            print("  note: entry scene is not present; import/author it before building")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        info = plan(args)
        if args.dry_run:
            _emit(info, args.json)
            return 0
        proc = subprocess.run(info["native_command"], capture_output=True, text=True, check=False)
        if proc.returncode:
            raise InitError(f"native Project init failed (exit {proc.returncode}): "
                            + (proc.stderr or proc.stdout).strip())
        root = Path(info["root"])
        name = info["project"]
        entry = info["entry_scene"]
        # Native init owns marker/dirs/build settings; only seed files
        # after it has completed successfully. Never replace authored files.
        if info["recipe"]:
            Path(info["recipe"]).open("x", encoding="utf-8").write(
                _scaffold(Path(info["recipe"]), name, entry, info["pak_id"],
                          info["bake_gi"], info["bake_cubemaps"]))
        if args.readme:
            (root / "README.md").open("x", encoding="utf-8").write(_readme(info))
        if args.gitignore:
            (root / ".gitignore").open("x", encoding="utf-8").write(
                "# Tourmaline-generated and local build data\n"
                ".tourmaline/\npaks/\nbuild/\n*.tmp\n")
        info["scene_ready"] = (root / entry).is_file()
        info["status"] = "adopted" if info["existing_guid"] else "created"
        info["native_stdout"] = proc.stdout.strip()
        _emit(info, args.json)
        return 0
    except (InitError, OSError) as exc:
        print(f"project_init: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
