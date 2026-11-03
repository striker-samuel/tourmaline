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
import sys
import tempfile
from dataclasses import replace
from typing import Mapping

from dcc_plugins import available_plugins, load_plugin
from dcc_runner import run_local, validate_environment


def validate_gltf(path: Path) -> None:
    """Check the basic single-file glTF handoff before replacing published data."""
    with path.open("r", encoding="utf-8") as file:
        doc = json.load(file)
    asset = doc.get("asset") if isinstance(doc, dict) else None
    if not isinstance(asset, dict) or asset.get("version") != "2.0":
        raise ValueError("export did not produce a glTF 2.0 document")
    if not any(isinstance(doc.get(key), list) for key in ("scenes", "nodes", "meshes")):
        raise ValueError("export has no scene content")
    # Atomic single-file handoff: a .gltf has no GLB binary chunk. Buffers
    # must have embedded data URIs; images may use those or bufferViews.
    for group in ("buffers", "images"):
        entries = doc.get(group, [])
        if not isinstance(entries, list):
            raise ValueError("export has an invalid " + group + " array")
        for item in entries:
            if not isinstance(item, dict):
                raise ValueError("export has an invalid " + group + " entry")
            uri = item.get("uri")
            if group == "buffers" and not uri:
                raise ValueError("export has a buffer without an embedded data URI")
            if group == "images" and uri is None and "bufferView" not in item:
                raise ValueError("export has an image without embedded data or bufferView")
            if uri is not None and (not isinstance(uri, str) or not uri.startswith("data:")):
                raise ValueError(
                    "export contains external or invalid " + group
                    + " dependency: " + str(uri)
                    + " (use embedded glTF for this basic runner)")


def publish(source: Path, output: Path, runner: str, *,
            maya_exe: str | None = None,
            blender_exe: str | None = None,
            tool_dir: Path | None = None,
            dcc_exe: str | None = None,
            environment: Mapping[str, str] | None = None,
            timeout_seconds: float | None = None) -> None:
    source = source.expanduser().resolve(strict=True)
    output = output.expanduser().resolve()
    if source == output:
        raise ValueError("authoring document and published output must differ")
    if output.suffix.lower() != ".gltf":
        raise ValueError("the basic DCC publisher requires a .gltf output")
    plugin = load_plugin(runner)
    if not plugin.accepts(source):
        raise ValueError(
            f"{runner} plugin cannot export {source.suffix} "
            f"(accepts {', '.join(plugin.extensions)})")
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
        # Plugins create job descriptions; the runner owns the actual
        # process (environment/argv/cwd/timeout). No DCC-specific
        # subprocess branching in the publish transaction.
        default_exe = {"maya": maya_exe, "blender": blender_exe}.get(runner)
        job = plugin.build_job(
            source, staged, tools,
            executable=dcc_exe or default_exe,
            environment=validate_environment(environment or {}))
        if timeout_seconds is not None:
            job = replace(job, timeout_seconds=timeout_seconds)
        run_local(job)
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
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runner", help="Python plugin filename (maya, blender, ...)")
    parser.add_argument("--dcc-exe", help="DCC executable override (otherwise plugin default)")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="per-job environment override, repeatable")
    parser.add_argument("--timeout", type=float, help="max DCC process runtime in seconds")
    parser.add_argument("--list-runners", action="store_true",
                        help="list installed Python DCC export plugins")
    args = parser.parse_args(argv)
    if args.list_runners:
        for name in available_plugins():
            print(name)
        return 0
    if not args.source or not args.output or not args.runner:
        parser.error("--source, --output and --runner are required to publish")
    try:
        env = {}
        for entry in args.env:
            if "=" not in entry:
                raise ValueError("--env expects NAME=VALUE")
            name, value = entry.split("=", 1)
            env[name] = value
        publish(args.source, args.output, args.runner,
                dcc_exe=args.dcc_exe, environment=env,
                timeout_seconds=args.timeout)
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print("DCC publication failed:", exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
