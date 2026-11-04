#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export one fresh renderer screenshot for every saved performance scenario.

Uses the existing App --bench / TOURMALINE_CAPTURE mechanism; no Nsight or
Pillow dependency is needed. Run from the repository root.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from perf_harness import read_catalog, select_scenario


ROOT = Path(__file__).resolve().parents[1]
SAFE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*\Z")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=ROOT / "benchmarks/scenarios.json")
    p.add_argument("--exe", type=Path, required=True, help="built App executable (not Benchmark)")
    p.add_argument("--workdir", type=Path, help="App working directory; defaults to exe directory")
    p.add_argument("--output", type=Path, default=Path("out/scenario-screenshots"))
    p.add_argument("--scenario", action="append", help="capture only this ID (repeatable); default: all")
    p.add_argument("--capture-frame", type=int, default=48,
                   help="1-based capture frame (clamped below each scenario's frame budget)")
    p.add_argument("--timeout", type=float, default=600, help="per-scenario timeout in seconds")
    p.add_argument("--overwrite", action="store_true", help="replace existing per-scenario results")
    p.add_argument("--dry-run", action="store_true", help="print planned commands without launching App")
    return p


def screenshot_state(folder):
    return {p.name: (p.stat().st_size, p.stat().st_mtime_ns)
            for p in folder.glob("screenshot_*.png") if p.is_file()}


def plan(scenario, exe, workdir, output, capture_frame):
    scenario_id = scenario["id"]
    if not SAFE_ID.fullmatch(scenario_id):
        raise ValueError(f"unsafe scenario ID: {scenario_id!r}")
    source = scenario["source_args"]
    for i in range(0, len(source), 2):
        if source[i] in ("--pak", "--project"):
            path = Path(source[i + 1])
            resolved = path if path.is_absolute() else workdir / path
            if not resolved.is_file():
                raise FileNotFoundError(f"missing scene input: {resolved}")
    frames = scenario["frames"]
    if frames < 2:
        raise ValueError("screenshot capture requires at least two frames")
    frame = min(capture_frame, frames - 1)
    pins = dict(scenario["environment"])
    if "TOURMALINE_CAPTURE" in pins:
        raise ValueError("scenario cannot override TOURMALINE_CAPTURE")
    pins["TOURMALINE_CAPTURE"] = str(frame)
    env = {k: v for k, v in os.environ.items() if not k.startswith("TOURMALINE_")}
    env.update(pins)
    command = [str(exe), *source, "--disable_control", "--bench", str(frames),
               "--present", "immediate", *scenario.get("app_args", [])]
    return {"id": scenario_id, "command": command, "cwd": str(workdir),
            "frames": frames, "capture_frame": frame, "environment": pins,
            "directory": str(output / scenario_id), "_env": env}


def export_one(item, timeout, overwrite):
    directory = Path(item["directory"])
    if directory.exists() and not overwrite and any(directory.iterdir()):
        raise FileExistsError(f"output exists: {directory} (use --overwrite)")
    directory.mkdir(parents=True, exist_ok=True)
    exports = Path(item["cwd"]) / "exports"
    before = screenshot_state(exports)
    with (directory / "app.log").open("w", encoding="utf-8") as log:
        try:
            completed = subprocess.run(item["command"], cwd=item["cwd"],
                                       env=item["_env"], stdout=log,
                                       stderr=subprocess.STDOUT, timeout=timeout,
                                       check=False)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"App timed out after {timeout:g}s") from exc
    if completed.returncode:
        raise RuntimeError(f"App exited with {completed.returncode}; see app.log")
    after = screenshot_state(exports)
    fresh = [exports / name for name, stat in after.items()
             if before.get(name) != stat]
    if not fresh:
        raise RuntimeError("App did not produce a fresh screenshot_*.png")
    latest = max(fresh, key=lambda p: (p.stat().st_mtime_ns, p.name))
    with latest.open("rb") as image:
        if image.read(8) != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError(f"invalid PNG from App: {latest}")
    shutil.copy2(latest, directory / "capture.png")
    return str(directory / "capture.png")


def main(argv=None):
    args = parser().parse_args(argv)
    if args.capture_frame < 1 or args.timeout <= 0:
        parser().error("--capture-frame and --timeout must be positive")
    exe = args.exe.expanduser().resolve()
    workdir = (args.workdir or exe.parent).expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not args.dry_run and (not exe.is_file() or not workdir.is_dir()):
        parser().error("App executable and working directory must exist")
    try:
        scenarios = read_catalog(args.catalog)
        ids = [s["id"] for s in scenarios] if not args.scenario else list(dict.fromkeys(args.scenario))
        scenarios = [select_scenario(args.catalog, scenario_id) for scenario_id in ids]
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser().error(str(exc))

    report = {"catalog": str(args.catalog.resolve()), "executable": str(exe),
              "output": str(output), "dry_run": args.dry_run, "scenarios": []}
    failed = False
    for scenario in scenarios:
        entry = {"id": scenario["id"]}
        try:
            # Dry-run can inspect command construction before installing scene content.
            if args.dry_run:
                entry = {"id": scenario["id"], "command": [
                    str(exe), *scenario["source_args"], "--disable_control", "--bench",
                    str(scenario["frames"]), "--present", "immediate",
                    *scenario.get("app_args", [])],
                    "environment": {**scenario["environment"],
                                    "TOURMALINE_CAPTURE": str(min(args.capture_frame, scenario["frames"] - 1))},
                    "directory": str(output / scenario["id"]), "status": "planned"}
                if not SAFE_ID.fullmatch(scenario["id"]) or scenario["frames"] < 2:
                    raise ValueError("invalid scenario ID or insufficient frame budget")
            else:
                item = plan(scenario, exe, workdir, output, args.capture_frame)
                entry["image"] = export_one(item, args.timeout, args.overwrite)
                entry["capture_frame"] = item["capture_frame"]
                entry["status"] = "captured"
        except (OSError, ValueError, RuntimeError) as exc:
            entry["status"] = "failed"
            entry["error"] = str(exc)
            failed = True
        report["scenarios"].append(entry)
        print(f"{entry['id']}: {entry['status']}" +
              (f" ({entry['error']})" if "error" in entry else ""), file=sys.stderr)

    if not args.dry_run:
        output.mkdir(parents=True, exist_ok=True)
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    else:
        print(json.dumps(report, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
