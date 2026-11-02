#!/usr/bin/env python3
"""Agent-friendly Tourmaline benchmark / Nsight Graphics launcher (stdlib only)."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

PERF = re.compile(r"\[perf\] avg frame ([0-9]+(?:\.[0-9]+)?) ms over ([0-9]+) frames")
SECTIONS = re.compile(r"\[perf\] sections poll ([0-9.]+) ms game ([0-9.]+) ms draw ([0-9.]+) ms")


def positive(value):
    n = int(value)
    if n < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def read_catalog(path):
    """Read the source-controlled scenario registry without executing code."""
    root = json.loads(path.read_text(encoding="utf-8"))
    if root.get("schema_version") != 1 or not isinstance(root.get("scenarios"), list):
        raise ValueError("unsupported scenario schema (expected version 1)")
    seen = set()
    for item in root["scenarios"]:
        if not isinstance(item, dict) or not isinstance(item.get("id"), str):
            raise ValueError("scenario must have a string id")
        if item["id"] in seen:
            raise ValueError(f"duplicate scenario id: {item['id']}")
        seen.add(item["id"])
    return root["scenarios"]


def select_scenario(catalog, scenario_id):
    items = [s for s in read_catalog(catalog) if s["id"] == scenario_id]
    if not items:
        raise ValueError(f"scenario '{scenario_id}' not found in {catalog}")
    item = items[0]
    src = item.get("source_args")
    pins = item.get("environment")
    extra = item.get("app_args", [])
    if not isinstance(src, list) or not src or not all(isinstance(x, str) for x in src):
        raise ValueError("source_args must be a nonempty string array")
    if not isinstance(pins, dict) or not all(
        isinstance(k, str) and re.fullmatch(r"TOURMALINE_[A-Z0-9_]+", k)
        and isinstance(v, str) for k, v in pins.items()
    ):
        raise ValueError("environment must contain TOURMALINE_* string pins")
    if not isinstance(extra, list) or not all(isinstance(x, str) for x in extra):
        raise ValueError("app_args must be an array of strings")
    if not isinstance(item.get("frames"), int) or item["frames"] < 1:
        raise ValueError("frames must be a positive integer")
    # Viewer exports --pak/--project, optional --scene/--scene-index.
    # Do not permit scenario data to smuggle in --bench or arbitrary flags.
    source_pairs = []
    i = 0
    while i < len(src):
        if src[i] not in ("--pak", "--project", "--scene", "--scene-index") or i + 1 >= len(src):
            raise ValueError("unsupported source_args option or missing value")
        source_pairs.append((src[i], src[i + 1]))
        i += 2
    sources = [p for flag, p in source_pairs if flag in ("--pak", "--project")]
    if not sources or ("--project" in src and len(sources) != 1):
        raise ValueError("a scenario needs at least one cooked pak or a project manifest")
    if any(p in ("", ".", "..") for p in sources):
        raise ValueError("empty or invalid scene path")
    return item


def command(args):
    # The exe requires its working directory to be the build root.
    source = getattr(args, "source_args", []) or (["--project", str(args.project.resolve())] if args.project else
           ["--pak", str(args.pak.resolve())])
    return [str(args.exe.resolve()), *source, "--disable_control", "--bench", str(args.frames),
            "--present", "immediate", *args.app_arg]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("bench", "trace", "capture", "compare", "list"))
    parser.add_argument("--exe", type=Path, help="Viewer App or dedicated Benchmark binary")
    parser.add_argument("--catalog", type=Path, default=Path(__file__).resolve().parents[1] / "benchmarks/scenarios.json")
    parser.add_argument("--scenario", help="Named source-controlled benchmark scenario id")
    scene = parser.add_mutually_exclusive_group()
    scene.add_argument("--pak", type=Path)
    scene.add_argument("--project", type=Path)
    parser.add_argument("--workdir", type=Path, help="App working directory (defaults to exe directory)")
    parser.add_argument("--output", type=Path, default=Path("out/perf"))
    parser.add_argument("--frames", type=positive, help="Measured frames; overrides scenario default (120 otherwise)")
    parser.add_argument("--repeat", type=positive, default=3)
    parser.add_argument("--warmup", type=positive, default=10, help="App internally uses 10 warmup frames")
    parser.add_argument("--capture-frame", type=positive, default=20)
    parser.add_argument("--trace-frames", type=positive, default=10, help="Nsight GPU Trace frames (1-15; default 10)")
    parser.add_argument("--trace-duration-ms", type=positive, default=10000, help="Maximum Nsight GPU Trace duration in ms")
    parser.add_argument("--ngfx", help="Path to Nsight Graphics ngfx[.exe] for GPU trace")
    parser.add_argument("--ngfx-capture", help="Path to ngfx-capture[.exe] for graphics capture")
    parser.add_argument("--platform", choices=("Windows", "Linux"), default="Windows" if os.name == "nt" else "Linux")
    parser.add_argument("--shader-debug", action="store_true", help="Require CMake shader-debug build evidence")
    parser.add_argument("--baseline", type=Path, help="JSON from previous bench (compare mode)")
    parser.add_argument("--candidate", type=Path, help="JSON from new bench (compare mode)")
    parser.add_argument("--max-regression", type=float, default=5.0, help="Fail comparison if slower by > this percentage")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE", help="Set a TOURMALINE_* pin")
    parser.add_argument("--app-arg", action="append", default=[], help="Additional App flag (repeat, one token per flag)")
    parser.add_argument("--timeout", type=positive, default=600)
    args = parser.parse_args(argv)

    if args.mode == "list":
        try:
            entries = read_catalog(args.catalog)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            parser.error(str(exc))
        print(json.dumps({"catalog": str(args.catalog), "scenarios": [
            {"id": s["id"], "label": s.get("label", ""), "subsystem": s.get("subsystem", ""),
             "frames": s.get("frames", 120)} for s in entries]}, indent=2))
        return 0

    if args.mode == "compare":
        if not args.baseline or not args.candidate:
            parser.error("compare requires --baseline and --candidate")
        a = json.loads(args.baseline.read_text(encoding="utf-8"))
        b = json.loads(args.candidate.read_text(encoding="utf-8"))
        if a.get("mode") != "bench" or b.get("mode") != "bench":
            parser.error("comparison requires two uninstrumented bench results")
        if a.get("frames") != b.get("frames") or a.get("app_args") != b.get("app_args") or a.get("environment") != b.get("environment") or a.get("source_args") != b.get("source_args") or a.get("scenario") != b.get("scenario"):
            parser.error("benchmark configurations differ; compare matching runs only")
        old, new = a["summary"]["median_ms"], b["summary"]["median_ms"]
        change = (new / old - 1) * 100
        print(json.dumps({"baseline_ms": old, "candidate_ms": new, "change_percent": change, "threshold_percent": args.max_regression, "regression": change > args.max_regression}, indent=2))
        return 2 if change > args.max_regression else 0

    scenario = None
    args.source_args = []
    if args.scenario:
        if args.pak or args.project:
            parser.error("--scenario is mutually exclusive with --pak/--project")
        try:
            scenario = select_scenario(args.catalog, args.scenario)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            parser.error(str(exc))
        args.source_args = scenario["source_args"]
        args.app_arg = scenario.get("app_args", []) + args.app_arg
        args.frames = args.frames or scenario["frames"]
    else:
        args.frames = args.frames or 120
        if not args.pak and not args.project:
            parser.error("--pak, --project or --scenario required")
    if args.exe is None:
        binary = "Benchmark.exe" if os.name == "nt" else "Benchmark"
        args.exe = Path("out/build/x64-Release") / (binary if scenario else ("App.exe" if os.name == "nt" else "App"))
    if not args.exe.is_file():
        parser.error(f"executable does not exist: {args.exe}")
    workdir = (args.workdir or args.exe.parent).resolve()
    if not workdir.is_dir():
        parser.error(f"working directory does not exist: {workdir}")
    if scenario:
        for i, flag in enumerate(args.source_args[:-1]):
            if flag in ("--pak", "--project"):
                source = Path(args.source_args[i + 1])
                if not (source if source.is_absolute() else workdir / source).is_file():
                    parser.error(f"scenario scene input missing relative to {workdir}: {source}")
    else:
        source = args.project or args.pak
        if not source.is_file():
            parser.error(f"scene input does not exist: {source}")
    if args.warmup != 10:
        parser.error("App currently has exactly 10 internal warmup frames")
    if args.mode == "capture" and (args.capture_frame < 2 or args.capture_frame >= args.frames + args.warmup):
        parser.error("capture frame must occur before the benchmark exits")
    if args.mode == "trace":
        if args.trace_frames > 15:
            parser.error("Nsight GPU Trace supports a maximum of 15 frames")
        if args.frames < args.trace_frames + 2:
            parser.error("benchmark measured frames must exceed --trace-frames by at least 2 so capture can finish")
    env = os.environ.copy()
    # One scenario must not inherit hidden TOURMALINE_* pins from the shell.
    # The full active environment is explicit and archived in result.json.
    if scenario:
        for key in list(env):
            if key.startswith("TOURMALINE_"):
                del env[key]
    pins = dict(scenario["environment"]) if scenario else {}
    for pair in args.env:
        key, sep, val = pair.partition("=")
        if not sep or not re.fullmatch(r"TOURMALINE_[A-Z0-9_]+", key):
            parser.error("--env requires TOURMALINE_NAME=value")
        pins[key] = val
    env.update(pins)
    # Prevent a user shell's shader hot reload from mutating the benchmark.
    env.pop("TOURMALINE_SHADER_RELOAD", None)
    env.pop("TOURMALINE_SHADER_RELOAD_SRC", None)
    args.output.mkdir(parents=True, exist_ok=True)
    out = args.output.resolve()
    argv_app = command(args)
    report = {"schema_version": 1, "mode": args.mode, "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "command": argv_app, "cwd": str(workdir), "frames": args.frames,
              "warmup_frames": args.warmup, "environment": pins, "app_args": args.app_arg,
              "source_args": args.source_args if scenario else argv_app[1:3], "scenario": args.scenario, "runs": []}
    if args.mode == "trace":
        report["trace_frames"] = args.trace_frames
        report["trace_duration_ms"] = args.trace_duration_ms
    if args.shader_debug:
        # Shader symbolication is a BUILD setting, not a runtime switch. Never claim
        # a release build contains SPIR-V line tables because this CLI was passed a flag.
        cache = workdir / "CMakeCache.txt"
        contents = cache.read_text(encoding="utf-8", errors="replace") if cache.is_file() else ""
        debug = re.search(r"^TOURMALINE_SHADER_DEBUG:BOOL=ON$", contents, re.MULTILINE)
        build = re.search(r"^CMAKE_BUILD_TYPE:STRING=Debug$", contents, re.MULTILINE)
        if not (debug or build):
            parser.error("--shader-debug requires a Debug build or CMake -DTOURMALINE_SHADER_DEBUG=ON (verified in CMakeCache.txt)")
        report["shader_debug_requested"] = True

    if args.mode == "bench":
        total = args.repeat
        invocations = [argv_app] * total
    elif args.mode == "trace":
        ngfx = args.ngfx or shutil.which("ngfx")
        if not ngfx:
            parser.error("GPU trace requires Nsight Graphics ngfx executable (--ngfx)")
        # Nsight Graphics GPU Trace CLI. Trace starts after a fixed number
        # of presents; samples are NOT a trustworthy standalone FPS benchmark.
        trace_dir = out / "nsight"
        trace_dir.mkdir(parents=True, exist_ok=True)
        invocations = [[ngfx, '--activity=GPU Trace Profiler', f'--platform={args.platform}',
                        f'--exe={args.exe.resolve()}', f'--dir={workdir}',
                        '--args=' + subprocess.list2cmdline(argv_app[1:]),
                        f'--output-dir={trace_dir}',
                        f'--start-after-frames={args.warmup}',
                        f'--limit-to-frames={args.trace_frames}',
                        f'--num-frames={args.trace_frames}',
                        f'--max-duration-ms={args.trace_duration_ms}', '--auto-export']]
    else:
        capture = args.ngfx_capture or shutil.which("ngfx-capture")
        if not capture:
            parser.error("graphics capture requires ngfx-capture executable (--ngfx-capture)")
        # Capture CLI syntax may differ across Nsight versions; check --help.
        invocations = [[capture, "--exe", str(args.exe.resolve()), "--working-dir", str(workdir),
                        "--args", subprocess.list2cmdline(argv_app[1:]),
                        "--capture-frame", str(args.capture_frame), "--frame-count", "1", "--output-dir", str(out)]]
    failed = False
    try:
        for i, invocation in enumerate(invocations, 1):
            print(f"[perf-harness] {args.mode} run {i}/{len(invocations)}", file=sys.stderr, flush=True)
            try:
                proc = subprocess.run(invocation, cwd=workdir, env=env, text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      timeout=args.timeout, errors="replace", check=False)
                log, rc = proc.stdout, proc.returncode
            except subprocess.TimeoutExpired as exc:
                log = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                rc = 124
            name = f"{args.mode}-{i:02}.log"
            (out / name).write_text(log, encoding="utf-8")
            item = {"exit_code": rc, "log": name}
            if args.mode == "bench":
                matches = PERF.findall(log)
                sections = SECTIONS.findall(log)
                if rc == 0 and len(matches) == 1 and int(matches[0][1]) == args.frames:
                    item["avg_frame_ms"] = float(matches[0][0])
                    if sections:
                        item["sections_ms"] = dict(zip(("poll", "game", "draw"), map(float, sections[-1])))
                else:
                    failed = True
                    item["error"] = "expected one matching [perf] summary and successful exit"
            elif rc != 0:
                failed = True
            report["runs"].append(item)
            print(json.dumps(item), flush=True)
            if failed:
                break
        if args.mode == "bench" and not failed:
            samples = [run["avg_frame_ms"] for run in report["runs"]]
            report["summary"] = {"median_ms": statistics.median(samples), "mean_ms": statistics.mean(samples),
                                 "min_ms": min(samples), "max_ms": max(samples),
                                 "estimated_fps": 1000 / statistics.median(samples)}
        report["ok"] = not failed
        if args.mode in ("capture", "trace"):
            report["note"] = "Nsight runs are instrumented; do not compare their FPS against bench mode. Inspect the Nsight output/log for capture artifacts."
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
