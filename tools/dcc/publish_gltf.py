#!/usr/bin/env python3
"""Tourmaline DCC build stage: authoring document -> validated published glTF.

The DCC owns the native .ma/.blend. Its runner exports an EMBEDDED glTF 2.0
into a staging directory; successful validation atomically replaces the
project's published .gltf. Tourmaline's content server then reimports it.

No shell commands, no direct engine artifact writes, no partial publish.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def validate_gltf(path: Path) -> None:
    with path.open("r", encoding="utf-8") as file:
        doc = json.load(file)
    if not isinstance(doc, dict) or not str(doc.get("asset", {}).get("version", "")).startswith("2."):
        raise ValueError("export did not produce a glTF 2.x document")
    if "scenes" not in doc and "nodes" not in doc and "meshes" not in doc:
        raise ValueError("export has no scene content")
    # Atomic single-file handoff: nothing should reference staging paths.
    for group in ("buffers", "images"):
        for item in doc.get(group, []):
            uri = item.get("uri", "")
            if uri and not uri.startswith("data:"):
                raise ValueError(
                    "export contains external " + group
                    + " dependency: " + uri
                    + " (use embedded glTF for this basic runner)")


def publish(source: Path, output: Path, runner: str, *,
            maya_exe: str | None = None,
            blender_exe: str | None = None,
            tool_dir: Path | None = None) -> None:
    source = source.expanduser().resolve(strict=True)
    output = output.expanduser().resolve()
    if source == output:
        raise ValueError("authoring document and published output must differ")
    if output.suffix.lower() != ".gltf":
        raise ValueError("the basic DCC publisher requires a .gltf output")
    if runner == "maya" and source.suffix.lower() not in (".ma", ".mb"):
        raise ValueError("Maya runner requires a .ma or .mb scene")
    if runner == "blender" and source.suffix.lower() != ".blend":
        raise ValueError("Blender runner requires a .blend scene")
    tools = tool_dir or Path(__file__).resolve().parent
    output.parent.mkdir(parents=True, exist_ok=True)
    # The native file watcher suppresses filenames containing ".tmp".
    # Stage a single embedded glTF sibling (same filesystem for atomic
    # replace) without a visible temporary directory in the asset browser.
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{output.stem}.tmp-", suffix=".gltf", dir=output.parent)
    os.close(fd)
    staged = Path(temp_name)
    staged.unlink()  # exporters write their own file
    try:
        if runner == "maya":
            executable = maya_exe or os.environ.get("TOURMALINE_MAYAPY", "mayapy")
            argv = [executable, str(tools / "maya_export_gltf.py"), str(source), str(staged)]
        elif runner == "blender":
            executable = blender_exe or os.environ.get("TOURMALINE_BLENDER", "blender")
            argv = [executable, "--background", str(source),
                    "--python", str(tools / "blender_export_gltf.py"),
                    "--", str(staged)]
        else:
            raise ValueError("unknown export runner: " + runner)
        completed = subprocess.run(argv, check=False)
        if completed.returncode:
            raise RuntimeError(f"{runner} exporter failed (exit {completed.returncode}); old publication intact")
        if not staged.is_file():
            raise RuntimeError("exporter returned success but produced no glTF; old publication intact")
        validate_gltf(staged)
        # os.replace is atomic within the same destination filesystem.
        os.replace(staged, output)
    finally:
        staged.unlink(missing_ok=True)
    print("Published glTF:", output, flush=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runner", required=True, choices=("maya", "blender"))
    args = parser.parse_args(argv)
    try:
        publish(args.source, args.output, args.runner)
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print("DCC publication failed:", exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
