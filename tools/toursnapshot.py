#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Agent-facing CLI for recording and validating Tourmaline PNG snapshots.

Run from any directory; 'record' launches an existing cooked App binary and
never loads arbitrary game code from PNGs. All other commands are GPU-free.

    python tools/toursnapshot.py record --app BUILD/App --project game.tapp --output out/frames --frames 123
    python tools/toursnapshot.py inspect out/frames --json
    python tools/toursnapshot.py verify out/frames --pixels --json
    python tools/toursnapshot.py extract out/frames --frame 61 --output state.json
    python tools/toursnapshot.py compare out/frames --frame 61 --candidate agent-state.json --json
    python tools/toursnapshot.py compare-sequence out/frames --candidate agent-frames --pixels --json
    python tools/toursnapshot.py input-tape out/frames --output replay.tinp

Exit 0: success/planned. Exit 1: IO, runtime, or invalid artifact.
Exit 2: a valid comparison differs. Exit 64: invalid CLI usage.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import snapshot_sequence as codec

SCHEMA_VERSION = 1
MAX_FRAMES = codec.MAX_FRAMES
MAX_TIMEOUT = 24 * 60 * 60
PIN = re.compile(r"TOURMALINE_[A-Z0-9_]+\Z")
FORBIDDEN_APP_ARGS = (
    "--bench", "--input-record", "--input-replay", "--snapshot-",
    "--project", "--pak", "--scene", "--scene-index", "--present",
)


class CliError(ValueError):
    """Invalid user argument. Exit 64, not a state mismatch (exit 2)."""


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise CliError(message)


def positive(raw: str) -> int:
    try:
        number = int(raw, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a positive integer") from exc
    if not 1 <= number <= MAX_FRAMES:
        raise argparse.ArgumentTypeError(f"expected 1..{MAX_FRAMES}")
    return number


def timeout_seconds(raw: str) -> int:
    number = positive(raw)
    if number > MAX_TIMEOUT:
        raise argparse.ArgumentTypeError("timeout exceeds one day")
    return number


def seed_number(raw: str) -> int:
    try:
        value = int(raw, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("seed must be a uint64 (decimal or 0xHEX)") from exc
    if not 0 <= value < 2 ** 64:
        raise argparse.ArgumentTypeError("seed out of uint64 range")
    return value


def parser_factory() -> Parser:
    parser = Parser(prog="toursnapshot", description=__doc__,
                    formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="action", required=True, parser_class=Parser)

    record = commands.add_parser("record", help="launch App and export verified Frame_NNNN.png files")
    record.add_argument("--app", type=Path, required=True, help="built App executable (not Benchmark)")
    sources = record.add_mutually_exclusive_group(required=True)
    sources.add_argument("--project", type=Path, help="cooked .tapp project manifest")
    sources.add_argument("--pak", type=Path, action="append", help="cooked .pak; repeat for multi-pak")
    sources.add_argument("--scenario", help="source-controlled benchmark scenario id")
    record.add_argument("--catalog", type=Path, default=Path(__file__).resolve().parents[1] /
                        "benchmarks/scenarios.json", help="scenario catalog JSON")
    record.add_argument("--scene", help="cooked named scene (with --pak only)")
    record.add_argument("--scene-index", type=int, help="scene index (with --pak only)")
    record.add_argument("--workdir", type=Path, help="App CWD, defaults to executable directory")
    record.add_argument("--output", type=Path, required=True, help="fresh Frame_*.png directory")
    record.add_argument("--frames", type=positive, help="exact number of PNGs (scenario default, else 120)")
    record.add_argument("--keyframe-interval", type=positive, default=60,
                        help="full state checkpoint every N frames (default 60)")
    record.add_argument("--seed", type=seed_number, help="PCG32 uint64 seed, default 0 unless scenario pins one")
    record.add_argument("--code-id", default="unspecified",
                        help="caller-supplied game-code build identity, NOT source code")
    record.add_argument("--output-format", choices=("png", "raw"), default="png",
                        help="PNG with embedded tsNp state (default), or RGBA8 + JSON sidecar")
    record.add_argument("--capture-mode", choices=("scoped", "legacy"), default="scoped",
                        help="15-frame GPU-resident batches by default; legacy synchronizes every frame")
    record.add_argument("--input-replay", type=Path, help="optional .tinp tape for replay-and-recapture")
    record.add_argument("--env", action="append", default=[], metavar="TOURMALINE_NAME=VALUE",
                        help="explicit renderer/environment pin (repeatable)")
    record.add_argument("--app-arg", action="append", default=[], metavar="ARG",
                        help="additional App option (use --app-arg=--minimal, repeat)")
    record.add_argument("--log", type=Path, help="non-overwriting App log, default OUTPUT/app.log")
    record.add_argument("--timeout", type=timeout_seconds, default=600, help="App timeout seconds")
    record.add_argument("--dry-run", action="store_true", help="print plan, do not launch/write files")
    record.add_argument("--pixels", action="store_true", help="also verify decoded PNG pixels (Pillow)")
    record.add_argument("--json", action="store_true", help="emit one JSON result on stdout")

    for name, desc in (
        ("inspect", "show keyframes, catalog provenance, RNG and state scope"),
        ("verify", "validate metadata, world hashes, PNG CRCs and optionally pixels"),
        ("extract", "reconstruct one frame's JSON world state"),
        ("compare", "compare agent JSON world state at a frame"),
        ("compare-sequence", "locate the first state/input/image divergence between two sequences"),
        ("input-tape", "export a standalone deterministic engine .tinp input tape"),
    ):
        sub = commands.add_parser(name, help=desc)
        sub.add_argument("sequence", type=Path, help="directory containing Frame_0001.png ...")
        sub.add_argument("--json", action="store_true", help="emit one JSON result on stdout")
        if name in ("inspect", "verify", "extract", "compare", "compare-sequence", "input-tape"):
            sub.add_argument("--pixels", action="store_true", help="verify decoded pixels (requires Pillow)")
        if name in ("extract", "compare"):
            sub.add_argument("--frame", required=True, type=positive, help="1-based frame number")
        if name in ("compare", "compare-sequence"):
            sub.add_argument("--candidate", type=Path, required=True,
                             help="agent state JSON (compare) or candidate PNG directory")
        if name in ("extract", "input-tape"):
            sub.add_argument("--output", type=Path, required=True,
                             help="new output file; refusing to overwrite")
    return parser


def emit(result: dict, json_mode: bool, *, exit_code: int = 0) -> int:
    payload = {"schema_version": SCHEMA_VERSION, **result}
    if json_mode:
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True, allow_nan=False))
    elif exit_code == 0:
        if result.get("status") == "planned":
            print("PLAN: " + " ".join(result["argv"]))
        else:
            print("OK: " + json.dumps(payload, ensure_ascii=False, sort_keys=True))
    elif exit_code == 2:
        print("DIFFERENT: " + json.dumps(payload, ensure_ascii=False, sort_keys=True))
    else:
        print("ERROR: " + str(result.get("message", payload)), file=sys.stderr)
    return exit_code


def summary(frames: list, *, pixels: bool) -> dict:
    first, last = frames[0][0], frames[-1][0]
    return {
        "status": "ok",
        "frames": len(frames),
        "sequence_id": first["sequence_id"],
        "first_tick": first["tick"],
        "last_tick": last["tick"],
        "keyframes": [i for i, (frame, _) in enumerate(frames, 1) if frame["kind"] == "key"],
        "pixels_checked": pixels,
    }


def _result_error(action: str, error: Exception | str, json_mode: bool,
                  *, extra: dict | None = None) -> int:
    return emit({"operation": action, "status": "error", "message": str(error), **(extra or {})},
                json_mode, exit_code=1)


def source_for(args, workdir: Path):
    """Build only documented cooked-source args (no shell interpolation)."""
    if args.scene and args.scene_index is not None:
        raise CliError("--scene and --scene-index are mutually exclusive")
    if (args.scene or args.scene_index is not None) and not args.pak:
        raise CliError("--scene/--scene-index require --pak")
    if args.scene_index is not None and not 0 <= args.scene_index < 2 ** 32:
        raise CliError("--scene-index must be a uint32")
    if args.project:
        return ["--project", str(args.project.resolve())], [], {}, None
    if args.pak:
        args_list = []
        for path in args.pak:
            args_list += ["--pak", str(path.resolve())]
        if args.scene:
            args_list += ["--scene", args.scene]
        if args.scene_index is not None:
            args_list += ["--scene-index", str(args.scene_index)]
        return args_list, [], {}, None
    # Reuse perf harness' strict scenario catalog validation. The scenario
    # only supplies cooked source arguments, pins, app args and frame count.
    import perf_harness
    try:
        item = perf_harness.select_scenario(args.catalog, args.scenario)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        raise CliError(f"invalid scenario {args.scenario!r}: {exc}") from exc
    return list(item["source_args"]), list(item.get("app_args", [])), dict(item["environment"]), item["frames"]


def validate_app_args(app_args: list[str]) -> None:
    for flag in app_args:
        if not isinstance(flag, str) or not flag:
            raise CliError("empty/non-string App argument")
        if any(flag == blocked or flag.startswith(blocked + "=")
               for blocked in FORBIDDEN_APP_ARGS if not blocked.endswith("-")) or any(
                flag.startswith(blocked) for blocked in FORBIDDEN_APP_ARGS if blocked.endswith("-")):
            raise CliError(f"forbidden App override in --app-arg / scenario: {flag}")


def _source_paths(argv: list[str], cwd: Path):
    for i, arg in enumerate(argv[:-1]):
        if arg in ("--pak", "--project"):
            p = Path(argv[i + 1])
            yield p if p.is_absolute() else cwd / p


def plan_record(args) -> tuple[dict, dict]:
    app = args.app.resolve()
    cwd = (args.workdir.resolve() if args.workdir is not None else app.parent)
    output = args.output.resolve()
    source, scenario_args, scenario_pins, scenario_frames = source_for(args, cwd)
    frames = args.frames or scenario_frames or 120
    if not 1 <= frames <= MAX_FRAMES:
        raise CliError(f"--frames must be 1..{MAX_FRAMES}")
    if len(args.code_id) > 256:
        raise CliError("--code-id may not exceed 256 characters")
    validate_app_args(scenario_args + args.app_arg)
    pins = scenario_pins.copy()
    for pair in args.env:
        name, sep, value = pair.partition("=")
        if not sep or not PIN.fullmatch(name):
            raise CliError("--env requires TOURMALINE_NAME=VALUE")
        pins[name] = value
    # Caller-supplied seed takes precedence over the scenario pin.
    if args.seed is not None:
        pins["TOURMALINE_RNG_SEED"] = str(args.seed)
    else:
        pins.setdefault("TOURMALINE_RNG_SEED", "0")
    replay = args.input_replay.resolve() if args.input_replay else None
    argv = [str(app), *source, f"--snapshot-sequence={output}",
            f"--snapshot-count={frames}",
            f"--snapshot-keyframe={args.keyframe_interval}",
            f"--snapshot-code-id={args.code_id}",
            f"--snapshot-capture={args.capture_mode}",
            f"--snapshot-format={args.output_format}",
            "--present", "immediate", *scenario_args, *args.app_arg]
    if replay:
        argv.append(f"--input-replay={replay}")
    log = args.log.resolve() if args.log else output / "app.log"
    result = {
        "operation": "record",
        "status": "planned",
        "sequence_dir": str(output),
        "frames": frames,
        "keyframe_interval": args.keyframe_interval,
        "code_identity": args.code_id,
        "seed": pins["TOURMALINE_RNG_SEED"],
        "app": str(app),
        "cwd": str(cwd),
        "log": str(log),
        "argv": argv,
        "environment_pins": dict(sorted(pins.items())),
        "pixels_checked": bool(args.pixels),
    }
    return result, {"app": app, "cwd": cwd, "output": output, "log": log,
                    "source": source, "replay": replay, "pins": pins, "argv": argv}


def run_record(args) -> dict:
    plan, paths = plan_record(args)
    if args.dry_run:
        return plan
    app, cwd, output, log = (paths[k] for k in ("app", "cwd", "output", "log"))
    if not app.is_file():
        raise codec.SnapshotError(f"App executable not found: {app}")
    if not cwd.is_dir():
        raise codec.SnapshotError(f"App working directory not found: {cwd}")
    for path in _source_paths(paths["source"], cwd):
        if not path.is_file():
            raise codec.SnapshotError(f"cooked project/pak not found: {path}")
    if paths["replay"] is not None and not paths["replay"].is_file():
        raise codec.SnapshotError(f"input tape not found: {paths['replay']}")
    if output.exists() and (not output.is_dir() or any(
            re.fullmatch(r"Frame_[0-9]+\.(?:png|rgba|json)", item.name) for item in output.iterdir())):
        raise codec.SnapshotError(f"output already contains a snapshot sequence: {output}")
    if log.exists():
        raise codec.SnapshotError(f"refusing to overwrite App log: {log}")
    output.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    # Keep stdout strictly machine-readable; App progress and diagnostics
    # always go to a log file, never through the agent CLI's stdout.
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("TOURMALINE_"):
            del env[key]
    env.pop("TOURMALINE_SHADER_RELOAD", None)
    env.pop("TOURMALINE_SHADER_RELOAD_SRC", None)
    env.update(paths["pins"])
    try:
        with log.open("x", encoding="utf-8") as log_stream:
            proc = subprocess.run(paths["argv"], cwd=cwd, env=env,
                                  stdout=log_stream, stderr=subprocess.STDOUT,
                                  timeout=args.timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise codec.SnapshotError(
            f"App timed out after {args.timeout}s (log: {log})") from exc
    except OSError as exc:
        raise codec.SnapshotError(f"failed to start App: {exc} (log: {log})") from exc
    if proc.returncode != 0:
        raise codec.SnapshotError(
            f"App exited {proc.returncode} (log: {log})")
    frames = codec.reconstruct(output, pixels=args.pixels)
    if len(frames) != plan["frames"]:
        raise codec.SnapshotError(
            f"expected {plan['frames']} frames; found {len(frames)} (log: {log})")
    expected = list(range(1, plan["frames"] + 1, args.keyframe_interval))
    actual = [i for i, (frame, _) in enumerate(frames, 1) if frame["kind"] == "key"]
    if actual != expected:
        raise codec.SnapshotError(
            f"keyframe interval mismatch; expected {expected}, got {actual}")
    result = summary(frames, pixels=args.pixels or args.output_format == "raw")
    result.update({"pixel_format": args.output_format, "operation": "record", "sequence_dir": str(output),
                   "app_exit_code": proc.returncode, "log": str(log),
                   "environment_pins": plan["environment_pins"]})
    return result


def _no_overwrite(path: Path):
    if path.exists() or path.is_symlink():
        raise codec.SnapshotError(f"refusing to overwrite output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)


def run_reader(args) -> tuple[dict, int]:
    frames = codec.reconstruct(args.sequence, pixels=args.pixels)
    result = summary(frames, pixels=args.pixels)
    result.update({"operation": args.action, "sequence_dir": str(args.sequence.resolve())})

    if args.action == "inspect":
        first = frames[0][0]
        result.update({"provenance": first["provenance"],
                       "state_scope": frames[0][1].get("scope", "unknown"),
                       "rng_seed": frames[0][1].get("rng", {}).get("seed"),
                       "first_state_fnv64": first["state_fnv64"],
                       "last_state_fnv64": frames[-1][0]["state_fnv64"]})
    elif args.action == "extract":
        if args.frame > len(frames):
            raise codec.SnapshotError(f"frame {args.frame} exceeds sequence length {len(frames)}")
        _no_overwrite(args.output)
        with args.output.open("xb") as output:
            output.write(codec.canonical(frames[args.frame - 1][1]) + b"\n")
        result.update({"frame": args.frame, "output": str(args.output.resolve())})
    elif args.action == "compare":
        if args.frame > len(frames):
            raise codec.SnapshotError(f"frame {args.frame} exceeds sequence length {len(frames)}")
        actual = codec._json(args.candidate.read_bytes())
        difference = codec._first_difference(frames[args.frame - 1][1], actual)
        if difference is not None:
            result.update({"status": "different", "frame": args.frame,
                           "type": "state", "first_difference": difference})
            return result, 2
        result["frame"] = args.frame
    elif args.action == "compare-sequence":
        candidate = codec.reconstruct(args.candidate, pixels=args.pixels)
        if len(frames) != len(candidate):
            result.update({"status": "different", "type": "frame_count",
                           "baseline_frames": len(frames), "candidate_frames": len(candidate)})
            return result, 2
        for index, ((left, state), (right, other)) in enumerate(zip(frames, candidate), 1):
            if left["tick"] != right["tick"]:
                result.update({"status": "different", "type": "tick", "frame": index})
                return result, 2
            if left["dt_ns"] != right["dt_ns"]:
                result.update({"status": "different", "type": "timestep", "frame": index})
                return result, 2
            if left["events"] != right["events"]:
                result.update({"status": "different", "type": "input", "frame": index})
                return result, 2
            difference = codec._first_difference(state, other)
            if difference is not None:
                result.update({"status": "different", "type": "state",
                               "frame": index, "first_difference": difference})
                return result, 2
            if left["pixels_fnv64"] != right["pixels_fnv64"]:
                result.update({"status": "different", "type": "pixels", "frame": index})
                return result, 2
        result["candidate_sequence_dir"] = str(args.candidate.resolve())
    elif args.action == "input-tape":
        _no_overwrite(args.output)
        codec.to_input_tape(frames, args.output)
        result["output"] = str(args.output.resolve())
    return result, 0


def main(argv=None) -> int:
    args_list = list(sys.argv[1:] if argv is None else argv)
    json_requested = "--json" in args_list
    try:
        args = parser_factory().parse_args(args_list)
        json_requested = args.json
        if args.action == "record":
            return emit(run_record(args), json_requested)
        result, code = run_reader(args)
        return emit(result, json_requested, exit_code=code)
    except CliError as exc:
        result = {"operation": args_list[0] if args_list else "usage",
                  "status": "error", "message": str(exc)}
        return emit(result, json_requested, exit_code=64) if json_requested else _usage_error(exc)
    except (codec.SnapshotError, OSError, ValueError, OverflowError, MemoryError,
            json.JSONDecodeError) as exc:
        action = args_list[0] if args_list else "unknown"
        return _result_error(action, exc, json_requested)


def _usage_error(error) -> int:
    print(f"toursnapshot: {error}", file=sys.stderr)
    print("Run 'python tools/toursnapshot.py --help' for usage.", file=sys.stderr)
    return 64


if __name__ == "__main__":
    raise SystemExit(main())
