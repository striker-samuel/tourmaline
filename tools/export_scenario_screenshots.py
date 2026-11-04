#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Batch-export scenario PNG sequences with embedded state and a JSON state dump.

Delegates recording, fresh-output protection, and validation to toursnapshot.
The scoped capture mode batches GPU readbacks (15 frames at a time).
"""

import argparse
import json
from pathlib import Path
import re
import sys

import perf_harness
import toursnapshot

ROOT = Path(__file__).resolve().parents[1]
SAFE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*\Z")


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=ROOT / "benchmarks/scenarios.json")
    p.add_argument("--app", type=Path, required=True,
                   help="built App executable (not Benchmark)")
    p.add_argument("--workdir", type=Path, help="App working directory (defaults to binary directory)")
    p.add_argument("--output", type=Path, default=Path("out/scenario-snapshots"))
    p.add_argument("--scenario", action="append", help="only this ID (repeatable); default: all")
    p.add_argument("--frames", type=int, help="override scenario frame budgets")
    p.add_argument("--state-frame", type=int, help="frame to extract (default: last frame)")
    p.add_argument("--keyframe-interval", type=int, default=60)
    p.add_argument("--code-id", default="unspecified", help="source/build identity for provenance")
    p.add_argument("--seed", type=int, help="override scenario RNG seed")
    p.add_argument("--capture-mode", choices=("scoped", "legacy"), default="scoped")
    p.add_argument("--timeout", type=int, default=600)
    p.add_argument("--pixels", action="store_true", help="also verify decoded pixels (Pillow)")
    p.add_argument("--dry-run", action="store_true", help="plan each scenario with no writes/GPU")
    return p


def run_scenario(args, scenario):
    scenario_id = scenario["id"]
    if not SAFE_ID.fullmatch(scenario_id):
        raise ValueError(f"unsafe scenario id: {scenario_id!r}")
    frames = args.frames if args.frames is not None else scenario["frames"]
    if frames < 1:
        raise ValueError("frame count must be positive")
    state_frame = args.state_frame if args.state_frame is not None else frames
    if not 1 <= state_frame <= frames:
        raise ValueError(f"state frame must be between 1 and {frames}")
    sequence = args.output.resolve() / scenario_id / "frames"
    command = ["record", "--app", str(args.app), "--scenario", scenario_id,
               "--catalog", str(args.catalog), "--output", str(sequence),
               "--frames", str(frames), "--keyframe-interval", str(args.keyframe_interval),
               "--code-id", args.code_id, "--capture-mode", args.capture_mode,
               "--timeout", str(args.timeout), "--json"]
    if args.workdir:
        command += ["--workdir", str(args.workdir)]
    if args.seed is not None:
        command += ["--seed", str(args.seed)]
    if args.pixels:
        command.append("--pixels")
    if args.dry_run:
        command.append("--dry-run")
    state_file = sequence.parent / "state.json"
    if not args.dry_run and state_file.exists():
        raise FileExistsError(f"refusing to overwrite state dump: {state_file}")
    record_args = toursnapshot.parser_factory().parse_args(command)
    result = toursnapshot.run_record(record_args)
    if args.dry_run:
        return {"id": scenario_id, "status": "planned", "record": result,
                "state_frame": state_frame, "state_file": str(sequence.parent / "state.json")}
    # The recorder already reconstructed and validated all PNG state chunks.
    # Reconstruct again to extract canonical state without shelling out.
    frames_data = toursnapshot.codec.reconstruct(sequence, pixels=args.pixels)
    # Own the scenario directory: run_record creates frames/ on success,
    # but a mocked or partially-failed record edge must not leave the
    # state write depending on it. (Never on the dry-run path - that
    # contract is zero filesystem side effects.)
    state_file.parent.mkdir(parents=True, exist_ok=True)
    state_file.write_bytes(toursnapshot.codec.canonical(frames_data[state_frame - 1][1]) + b"\n")
    return {"id": scenario_id, "status": "captured", "record": result,
            "state_frame": state_frame, "state_file": str(state_file)}


def main(argv=None):
    args = parser().parse_args(argv)
    if args.timeout <= 0 or args.keyframe_interval <= 0 or (
            args.frames is not None and args.frames <= 0) or (
            args.state_frame is not None and args.state_frame <= 0):
        parser().error("timeout, frame counts and keyframe interval must be positive")
    try:
        catalog = perf_harness.read_catalog(args.catalog)
        ids = ([s["id"] for s in catalog] if not args.scenario
               else list(dict.fromkeys(args.scenario)))
        scenarios = [perf_harness.select_scenario(args.catalog, sid) for sid in ids]
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser().error(str(exc))

    report = {"schema_version": 1, "catalog": str(args.catalog.resolve()),
              "output": str(args.output.resolve()), "dry_run": args.dry_run,
              "scenarios": []}
    failed = False
    for scenario in scenarios:
        try:
            entry = run_scenario(args, scenario)
        except (OSError, ValueError, toursnapshot.CliError,
                toursnapshot.codec.SnapshotError) as exc:
            entry = {"id": scenario["id"], "status": "failed", "error": str(exc)}
            failed = True
        report["scenarios"].append(entry)
        print(f"{entry['id']}: {entry['status']}" +
              (f" ({entry['error']})" if "error" in entry else ""), file=sys.stderr)
    if args.dry_run:
        print(json.dumps(report, indent=2))
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n",
                                                   encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
