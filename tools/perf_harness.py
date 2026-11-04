#!/usr/bin/env python3
"""Agent-friendly Tourmaline benchmark / Nsight Graphics launcher (stdlib only)."""
import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchmark_environment import sample_environment


def default_paths_config():
    # User-local settings, not in the Git repository or distributed content.
    if os.name == "nt":
        root = Path(os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local"))
    else:
        root = Path(os.environ.get("XDG_CONFIG_HOME") or (Path.home() / ".config"))
    return root / "Tourmaline" / "nsight_trace_paths.json"


def resolve_paths(parser, args):
    config_file = (args.paths_config or default_paths_config()).expanduser().resolve()
    chosen = {"schema_version": 1}
    if config_file.is_file():
        try:
            saved = json.loads(config_file.read_text(encoding="utf-8"))
            if not isinstance(saved, dict) or saved.get("schema_version") != 1:
                raise ValueError("unexpected preferences schema")
            for key in ("capture_dir", "scratch_dir"):
                if key in saved:
                    if not isinstance(saved[key], str):
                        raise ValueError("invalid directory in preferences")
                    chosen[key] = saved[key]
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            parser.error(f"cannot read Nsight preferences {config_file}: {exc}")
    if args.capture_dir is not None:
        chosen["capture_dir"] = str(args.capture_dir.expanduser().resolve())
    if args.scratch_dir is not None:
        chosen["scratch_dir"] = str(args.scratch_dir.expanduser().resolve())
    if args.save_paths:
        if args.capture_dir is None and args.scratch_dir is None:
            parser.error("--save-paths requires --capture-dir and/or --scratch-dir")
        try:
            config_file.parent.mkdir(parents=True, exist_ok=True)
            pending = config_file.with_suffix(".tmp")
            pending.write_text(json.dumps(chosen, indent=2) + chr(10), encoding="utf-8")
            pending.replace(config_file)
        except OSError as exc:
            parser.error(f"cannot save Nsight preferences: {exc}")
    return config_file, chosen


PERF = re.compile(r"\[perf\] avg frame ([0-9]+(?:\.[0-9]+)?) ms over ([0-9]+) frames")
SECTIONS = re.compile(r"\[perf\] sections poll ([0-9.]+) ms game ([0-9.]+) ms draw ([0-9.]+) ms")
CULL_DUMP_WROTE = re.compile(r"\[CullDump\] wrote (\S+)")
CULL_DUMP_FAILED = re.compile(r"\[CullDump\] FAILED: (.*)")


def cull_baseline_compare(dump, baseline_path, update):
    """Record or compare a cull dump against its tracked baseline.

    Returns (status, differs) where status is one of:
      "updated"  - --update wrote the dump as the new baseline
      "match"    - the dump equals the tracked baseline
      "mismatch" - differs; `differs` lists the differing top-level keys
      "missing"  - no baseline recorded yet (run with --update first)
    """
    if update:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(dump, indent=2) + "\n", encoding="utf-8")
        return "updated", None
    if not baseline_path.is_file():
        return "missing", None
    base = json.loads(baseline_path.read_text(encoding="utf-8"))
    if base == dump:
        return "match", None
    differs = sorted(k for k in set(base) | set(dump) if base.get(k) != dump.get(k))
    return "mismatch", differs

# Resolution scaling study: the headless window is the FrameConfig default
# 1920x1080 (engine/include/Engine/RHI/FrameConfig.h WindowDefaultWidth/
# Height) and TOURMALINE_RENDER_SCALE re-sizes the RENDER extent (lround per
# axis, engine clamp 0.25..2.0). One scale per run => the presets below are
# exact (16:9 keeps per-axis scales identical).
RESOLUTION_PRESETS = {
    "720p": (1280, 720),
    "1080p": (1920, 1080),
    "1440p": (2560, 1440),
    "2160p": (3840, 2160),
    "4k": (3840, 2160),
}
NATIVE_WINDOW = (1920, 1080)

# Warm-run priming: the first invocation after any shader/pipeline change
# pays pipeline compilation (and first-use driver caches) INSIDE its frames
# - observed as 6.2 ms vs 1.6 ms medians on identical content. Bench mode
# therefore runs one UNMEASURED priming invocation per distinct environment
# first; the disk pipeline-cache save happens at process shutdown, outside
# the measured window, so a primed measured run compiles and saves nothing.
PRIMING_BENCH_FRAMES = 12  # the App's 10 internal warmup frames + 2: every pass executes once


def priming_command(argv_app, frames=PRIMING_BENCH_FRAMES):
    """The measured command with a minimal --bench budget: enough frames to
    compile every pipeline and populate the caches, cheap enough to throw
    away. Priming runs are never summarized."""
    out = list(argv_app)
    if "--bench" not in out:
        raise ValueError("bench command missing --bench")
    out[out.index("--bench") + 1] = str(frames)
    return out


def priming_targets(plan):
    """One priming run per DISTINCT environment in the plan. Pipelines are
    environment-shaped (MSAA changes render passes and sample counts; the
    render extent does not, but a per-environment priming run is cheap and
    bulletproof against future extent-keyed state)."""
    seen, targets = [], []
    keys = set()
    for item in plan:
        key = tuple(sorted(item["env"].items()))
        if key not in keys:
            keys.add(key)
            targets.append(item)
    return targets


def parse_resolutions(spec):
    """'720p,1080p,1440p,4k' (or explicit 16:9 WxH) -> ordered run descriptors."""
    out, seen = [], set()
    for token in spec.split(","):
        token = token.strip().lower()
        if not token:
            continue
        if token in RESOLUTION_PRESETS:
            width, height = RESOLUTION_PRESETS[token]
        elif re.fullmatch(r"\d{3,5}x\d{3,5}", token):
            width, height = (int(v) for v in token.split("x"))
        else:
            raise ValueError(
                f"unknown resolution '{token}' (presets: {sorted(set(RESOLUTION_PRESETS))} or WxH)")
        if width * NATIVE_WINDOW[1] != height * NATIVE_WINDOW[0]:
            raise ValueError(
                f"resolution {token} is not 16:9: the engine's single "
                "TOURMALINE_RENDER_SCALE cannot express it against the 1920x1080 window")
        scale = height / NATIVE_WINDOW[1]
        if not 0.25 <= scale <= 2.0:
            raise ValueError(
                f"resolution {token} maps to render scale {scale:.3f}, "
                "outside the engine's 0.25..2.0 clamp")
        if token not in seen:
            seen.add(token)
            out.append({"label": token, "width": width, "height": height,
                        "render_scale": scale})
    if not out:
        raise ValueError("empty --resolutions list")
    return out


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
    if getattr(args, "mode", "") == "snapshot":
        dest = (args.output / "sequence").resolve()
        return [str(args.exe.resolve()), *source,
                f"--snapshot-sequence={dest}",
                f"--snapshot-count={args.frames}",
                f"--snapshot-keyframe={args.keyframe_interval}",
                f"--snapshot-code-id={args.code_id or 'unspecified'}",
                "--present", "immediate", *args.app_arg]
    return [str(args.exe.resolve()), *source, "--disable_control", "--bench", str(args.frames),
            "--present", "immediate", *args.app_arg]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("bench", "trace", "capture", "compare", "list", "cull", "snapshot"))
    parser.add_argument("--exe", type=Path, help="Viewer App or dedicated Benchmark binary")
    parser.add_argument("--catalog", type=Path, default=Path(__file__).resolve().parents[1] / "benchmarks/scenarios.json")
    parser.add_argument("--scenario", help="Named source-controlled benchmark scenario id")
    scene = parser.add_mutually_exclusive_group()
    scene.add_argument("--pak", type=Path)
    scene.add_argument("--project", type=Path)
    parser.add_argument("--workdir", type=Path, help="App working directory (defaults to exe directory)")
    parser.add_argument("--output", type=Path, default=Path("out/perf"), help="Run logs and result.json")
    parser.add_argument("--capture-dir", type=Path, help="Root folder for Nsight exports (overrides saved preference)")
    parser.add_argument("--scratch-dir", type=Path, help="Fast disk for scratch (TMP, TEMP, TMPDIR; Linux XDG_CACHE_HOME)")
    parser.add_argument("--paths-config", type=Path, help="Alternative user-local preferences JSON")
    parser.add_argument("--save-paths", action="store_true", help="Persist capture and scratch settings")
    parser.add_argument("--show-paths", action="store_true", help="Display current paths without starting Nsight")
    parser.add_argument("--dry-run", action="store_true", help="Print the planned Nsight command and destinations without running it")
    parser.add_argument("--frames", type=positive, help="Measured frames (snapshot mode: exactly this many PNGs)")
    parser.add_argument("--keyframe-interval", type=positive, default=60,
                        help="snapshot mode: full world checkpoint every N frames")
    parser.add_argument("--code-id", default="",
                        help="snapshot mode: BYOC game/code commit or content hash (no source embedded)")
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
    parser.add_argument("--resolutions", help="bench only: comma list of presets (720p,1080p,1440p,4k) or 16:9 WxH "
                          "values - one repeat-set per resolution; MSAA 2x is the default AA load")
    parser.add_argument("--cull-frame", type=positive, default=12,
                        help="cull mode: the 1-based app frame the engine dumps verdicts at (TOURMALINE_CULL_DUMP)")
    parser.add_argument("--update", action="store_true",
                        help="cull mode: record the dump as the scenario's tracked baseline instead of comparing")
    parser.add_argument("--baseline-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "benchmarks/cull_baselines",
                        help="cull mode: the tracked baseline directory (one <scenario>.json per camera)")
    parser.add_argument("--app-arg", action="append", default=[], help="Additional App flag (repeat, one token per flag)")
    parser.add_argument("--timeout", type=positive, default=600)
    args = parser.parse_args(argv)
    if args.mode in ("trace", "capture") or args.save_paths or args.show_paths:
        config_file, chosen = resolve_paths(parser, args)
        if args.save_paths or args.show_paths:
            print(json.dumps({"config": str(config_file),
                              "capture_dir": chosen.get("capture_dir", str((args.output / "nsight").resolve())),
                              "scratch_dir": chosen.get("scratch_dir", "system temp"),
                              "saved": args.save_paths}, indent=2))
            return 0
        args.capture_dir = Path(chosen.get("capture_dir") or (args.output / "nsight")).expanduser().resolve()
        args.scratch_dir = Path(chosen["scratch_dir"]).expanduser().resolve() if chosen.get("scratch_dir") else None
        # Keep reports and logs on the chosen capture drive by default,
        # not just the larger Nsight trace artifacts.
        if chosen.get("capture_dir") and args.output == Path("out/perf"):
            args.output = args.capture_dir / "_runs"
    else:
        args.capture_dir = None
        args.scratch_dir = None

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
        if a.get("ok") is False or b.get("ok") is False or a.get("environment_checks", [{}])[-1].get("status") not in ("clean", None) or b.get("environment_checks", [{}])[-1].get("status") not in ("clean", None):
            parser.error("cannot compare invalid or contaminated benchmark results")
        if a.get("frames") != b.get("frames") or a.get("app_args") != b.get("app_args") or a.get("environment") != b.get("environment") or a.get("source_args") != b.get("source_args") or a.get("scenario") != b.get("scenario"):
            parser.error("benchmark configurations differ; compare matching runs only")
        # Resolution-scaling results compare per resolution: a regression at
        # ANY studied resolution fails the comparison (scaling behavior is
        # exactly what these runs exist to protect).
        if "resolutions" in a and "resolutions" in b:
            la = [r["label"] for r in a["resolutions"]]
            lb = [r["label"] for r in b["resolutions"]]
            if la != lb:
                parser.error("resolution lists differ; compare identical --resolutions runs")
            rows, worst = [], -100.0
            for ra, rb in zip(a["resolutions"], b["resolutions"]):
                old, new = ra["summary"]["median_ms"], rb["summary"]["median_ms"]
                change = (new / old - 1) * 100
                worst = max(worst, change)
                rows.append({"resolution": ra["label"], "baseline_ms": old,
                             "candidate_ms": new, "change_percent": change})
            regression = worst > args.max_regression
            print(json.dumps({"resolutions": rows, "threshold_percent": args.max_regression,
                              "worst_change_percent": worst, "regression": regression}, indent=2))
            return 2 if regression else 0
        old, new = a["summary"]["median_ms"], b["summary"]["median_ms"]
        change = (new / old - 1) * 100
        print(json.dumps({"baseline_ms": old, "candidate_ms": new, "change_percent": change, "threshold_percent": args.max_regression, "regression": change > args.max_regression}, indent=2))
        return 2 if change > args.max_regression else 0

    resolutions = []
    if args.resolutions:
        if args.mode != "bench":
            parser.error("--resolutions applies to bench mode only")
        try:
            resolutions = parse_resolutions(args.resolutions)
        except ValueError as exc:
            parser.error(str(exc))

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
        args.frames = args.frames or (args.trace_frames + 2 if args.mode == "trace" else scenario["frames"])
    else:
        args.frames = args.frames or (args.trace_frames + 2 if args.mode == "trace" else 120)
        if not args.pak and not args.project:
            parser.error("--pak, --project or --scenario required")
    if args.exe is None:
        binary = "Benchmark.exe" if os.name == "nt" else "Benchmark"
        host = "App.exe" if os.name == "nt" else "App"
        args.exe = Path("out/build/x64-Release") / (host if args.mode == "snapshot" else (binary if scenario else host))
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
    user_keys = set()
    for pair in args.env:
        key, sep, val = pair.partition("=")
        if not sep or not re.fullmatch(r"TOURMALINE_[A-Z0-9_]+", key):
            parser.error("--env requires TOURMALINE_NAME=value")
        pins[key] = val
        user_keys.add(key)
    if resolutions and "TOURMALINE_AA" not in user_keys:
        # The scaling study's canonical load is MSAA 2x: it deliberately
        # overrides a scenario's AA pin (the sponza-atrium canon pins AA=off
        # for golden determinism). An explicit --env TOURMALINE_AA=... wins.
        pins["TOURMALINE_AA"] = "msaa2x"
    if args.mode == "snapshot" and "TOURMALINE_RNG_SEED" not in pins:
        # In a replayable scenario, do not depend on process entropy.
        pins["TOURMALINE_RNG_SEED"] = "0"
    env.update(pins)
    if args.mode == "cull":
        if not args.scenario:
            parser.error("cull mode requires --scenario (baselines are per saved camera)")
        if args.resolutions:
            parser.error("--resolutions applies to bench mode only")
        # Arm the engine's verdict dump for the chosen frame. The pin rides
        # the environment so result.json records exactly what was armed.
        pins["TOURMALINE_CULL_DUMP"] = str(args.cull_frame)
        env["TOURMALINE_CULL_DUMP"] = str(args.cull_frame)
    if args.scratch_dir is not None:
        if not args.dry_run:
            try:
                args.scratch_dir.mkdir(parents=True, exist_ok=True)
                if not args.scratch_dir.is_dir():
                    raise OSError("scratch path is not a directory")
            except OSError as exc:
                parser.error(f"invalid scratch directory {args.scratch_dir}: {exc}")
        # Nsight GPU Trace does not document a dedicated scratch-dir flag.
        # Let child/descendant processes resolve OS temp files onto this disk.
        # Internal NVIDIA caches may still use their own fixed locations.
        for name in ("TMP", "TEMP", "TMPDIR"):
            env[name] = str(args.scratch_dir)
        if os.name != "nt":
            env["XDG_CACHE_HOME"] = str(args.scratch_dir)
    # Prevent a user shell's shader hot reload from mutating the benchmark.
    env.pop("TOURMALINE_SHADER_RELOAD", None)
    env.pop("TOURMALINE_SHADER_RELOAD_SRC", None)
    if not args.dry_run:
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

    if args.mode == "snapshot":
        report["snapshot_dir"] = str(out / "sequence")
        report["keyframe_interval"] = args.keyframe_interval
        # One unprimed App run: snapshots capture gameplay rather than
        # benchmark's stripped extension stack and 10 warmup frames.
        plan = [{"invocation": argv_app, "env": env,
                 "name": "snapshot-01.log", "resolution": None}]
    elif args.mode == "bench":
        if resolutions:
            # One repeat-set per resolution: the render scale rides the
            # environment (never the command line), so each run's log and
            # result entry records exactly which extent it measured.
            # ASSET-QUALITY PIN: the texture streamer's wanted-mip demand
            # scales with the render extent (higher resolutions legitimately
            # demand higher-quality textures). TOURMALINE_TEXSTREAM_MIP_BIAS
            # = log2(scale) cancels that, so every studied resolution demands
            # the SAME asset quality as the 1080p reference and the scaling
            # table measures pixels, not asset work. An explicit --env pin wins.
            plan = []
            for res in resolutions:
                res_env = dict(env)
                res_env["TOURMALINE_RENDER_SCALE"] = f"{res['render_scale']:.6g}"
                if "TOURMALINE_TEXSTREAM_MIP_BIAS" not in user_keys:
                    res_env["TOURMALINE_TEXSTREAM_MIP_BIAS"] = f"{math.log2(res['render_scale']):.3f}"
                for i in range(1, args.repeat + 1):
                    plan.append({"invocation": argv_app, "env": res_env,
                                 "name": f"bench-{res['label']}-{i:02}.log",
                                 "resolution": res})
        else:
            plan = [{"invocation": argv_app, "env": env,
                     "name": f"bench-{i:02}.log", "resolution": None}
                    for i in range(1, args.repeat + 1)]
    elif args.mode == "trace":
        ngfx = args.ngfx or shutil.which("ngfx")
        if not ngfx and not args.dry_run:
            parser.error("GPU trace requires Nsight Graphics ngfx executable (--ngfx)")
        ngfx = ngfx or "ngfx"
        # Nsight Graphics GPU Trace CLI. Trace starts after a fixed number
        # of presents; samples are NOT a trustworthy standalone FPS benchmark.
        trace_root = args.capture_dir or (out / "nsight")
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        slug = re.sub(r"[^a-zA-Z0-9_-]", "_", args.scenario or "manual")
        trace_dir = trace_root / f"{slug}-{stamp}"
        if not args.dry_run:
            try:
                trace_dir.mkdir(parents=True, exist_ok=False)
            except OSError as exc:
                parser.error(f"cannot create capture directory {trace_dir}: {exc}")
        report["capture_dir"] = str(trace_dir)
        report["scratch_dir"] = str(args.scratch_dir) if args.scratch_dir else None
        target_pins = dict(pins)
        if args.scratch_dir is not None:
            target_pins.update({k: str(args.scratch_dir) for k in ("TMP", "TEMP", "TMPDIR")})
        plan = [{"invocation": [ngfx, '--activity=GPU Trace Profiler', f'--platform={args.platform}',
                                f'--exe={args.exe.resolve()}', f'--dir={workdir}',
                                '--args=' + subprocess.list2cmdline(argv_app[1:]),
                                f'--output-dir={trace_dir}',
                                f'--start-after-frames={args.warmup}',
                                f'--limit-to-frames={args.trace_frames}',
                                f'--num-frames={args.trace_frames}',
                                f'--max-duration-ms={args.trace_duration_ms}', '--auto-export',
                                *(['--env=' + '; '.join(f'{k}={v}' for k, v in target_pins.items())] if target_pins else [])],
                 "env": env, "name": "trace-01.log", "resolution": None}]
    elif args.mode == "cull":
        # One invocation at the scenario camera; the engine's
        # TOURMALINE_CULL_DUMP pin (armed above) writes the verdict dump.
        plan = [{"invocation": argv_app, "env": env,
                 "name": "cull-01.log", "resolution": None}]
    else:
        capture = args.ngfx_capture or shutil.which("ngfx-capture")
        if not capture:
            parser.error("graphics capture requires ngfx-capture executable (--ngfx-capture)")
        # Capture CLI syntax may differ across Nsight versions; check --help.
        cap_root = args.capture_dir or out
        cap_root.mkdir(parents=True, exist_ok=True)
        report["capture_dir"] = str(cap_root)
        report["scratch_dir"] = str(args.scratch_dir) if args.scratch_dir else None
        plan = [{"invocation": [capture, "--exe", str(args.exe.resolve()), "--working-dir", str(workdir),
                                "--args", subprocess.list2cmdline(argv_app[1:]),
                                "--capture-frame", str(args.capture_frame), "--frame-count", "1", "--output-dir", str(cap_root)],
                 "env": env, "name": "capture-01.log", "resolution": None}]
    if args.dry_run:
        print(json.dumps({"mode": args.mode, "command": plan[0]["invocation"],
                          "cwd": str(workdir), "capture_dir": report.get("capture_dir"),
                          "scratch_dir": str(args.scratch_dir) if args.scratch_dir else "system temp"}, indent=2))
        return 0
    failed = False
    # Only raw frame-time benchmarks require exclusivity. A check is also
    # taken after every subprocess so contention during measurement is fatal.
    exclusive = args.mode == "bench"
    report["environment_checks"] = []
    def check_clean(phase):
        nonlocal failed
        sample = sample_environment()
        sample["phase"] = phase
        report["environment_checks"].append(sample)
        if sample["status"] != "clean":
            failed = True
            report["invalid_reason"] = "unclean benchmark environment at " + phase
            print("[perf-harness] ERROR: " + report["invalid_reason"] +
                  ": " + "; ".join(sample["reasons"]), file=sys.stderr, flush=True)
            return False
        return True

    try:
        if exclusive:
            check_clean("preflight")
        # Warm runs first (bench only): compilation and first-use cache
        # variance land in priming invocations that are never summarized.
        priming = []
        if args.mode == "bench" and not failed:
            targets = priming_targets(plan)
            for j, target in enumerate(targets, 1):
                tag = f" [{target['resolution']['label']}]" if target["resolution"] else ""
                print(f"[perf-harness] priming run {j}/{len(targets)}{tag} (unmeasured)",
                      file=sys.stderr, flush=True)
                name = (f"warmup-{target['resolution']['label']}.log"
                        if target["resolution"] else f"warmup-{j:02}.log")
                try:
                    proc = subprocess.run(priming_command(target["invocation"]),
                                          cwd=workdir, env=target["env"], text=True,
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                          timeout=args.timeout, errors="replace", check=False)
                    log, rc = proc.stdout, proc.returncode
                except subprocess.TimeoutExpired as exc:
                    log = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                    rc = 124
                (out / name).write_text(log, encoding="utf-8")
                entry = {"exit_code": rc, "log": name}
                if rc != 0:
                    failed = True
                    entry["error"] = "priming run failed (the measured runs never started)"
                priming.append(entry)
                print(json.dumps(entry), flush=True)
                if not failed and exclusive:
                    check_clean("after priming " + str(j))
                if failed:
                    break
        report["priming"] = priming
        for i, item_plan in enumerate(plan, 1):
            if failed:
                break
            if exclusive and not check_clean("before run " + str(i)):
                break
            print(f"[perf-harness] {args.mode} run {i}/{len(plan)}"
                  + (f" [{item_plan['resolution']['label']}]" if item_plan["resolution"] else ""),
                  file=sys.stderr, flush=True)
            invocation, run_env, name = item_plan["invocation"], item_plan["env"], item_plan["name"]
            try:
                proc = subprocess.run(invocation, cwd=workdir, env=run_env, text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      timeout=args.timeout, errors="replace", check=False)
                log, rc = proc.stdout, proc.returncode
            except subprocess.TimeoutExpired as exc:
                log = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                rc = 124
            (out / name).write_text(log, encoding="utf-8")
            item = {"exit_code": rc, "log": name}
            if item_plan["resolution"]:
                item["resolution"] = item_plan["resolution"]["label"]
            if args.mode in ("bench", "cull"):
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
            if args.mode == "snapshot" and rc == 0:
                try:
                    import snapshot_sequence
                    frames = snapshot_sequence.reconstruct(out / "sequence")
                    if len(frames) != args.frames:
                        raise snapshot_sequence.SnapshotError(
                            f"expected {args.frames} PNGs; received {len(frames)}")
                    item["snapshots"] = len(frames)
                    item["keyframes"] = [i + 1 for i, (r, _) in enumerate(frames)
                                         if r["kind"] == "key"]
                    item["first_tick"] = frames[0][0]["tick"]
                except (snapshot_sequence.SnapshotError, OSError, ValueError) as exc:
                    failed = True
                    item["error"] = f"invalid/missing snapshot sequence: {exc}"
            report["runs"].append(item)
            if exclusive and not check_clean("after run " + str(i)):
                item["error"] = report["invalid_reason"]
                item.pop("avg_frame_ms", None)
            print(json.dumps(item), flush=True)
            if failed:
                break
        if args.mode == "bench" and not failed:
            def summarize(runs):
                samples = [run["avg_frame_ms"] for run in runs]
                return {"median_ms": statistics.median(samples), "mean_ms": statistics.mean(samples),
                        "min_ms": min(samples), "max_ms": max(samples),
                        "estimated_fps": 1000 / statistics.median(samples)}
            if resolutions:
                # Per-resolution groups + the scaling table (the study's
                # deliverable: how render time grows with the extent).
                report["resolutions"] = []
                table = []
                for res in resolutions:
                    runs = [r for r in report["runs"] if r.get("resolution") == res["label"]]
                    bias_used = (pins["TOURMALINE_TEXSTREAM_MIP_BIAS"]
                                 if "TOURMALINE_TEXSTREAM_MIP_BIAS" in user_keys
                                 else f"{math.log2(res['render_scale']):.3f}")
                    entry = {"label": res["label"], "width": res["width"], "height": res["height"],
                             "render_scale": res["render_scale"],
                             "environment": {"TOURMALINE_RENDER_SCALE": f"{res['render_scale']:.6g}",
                                             "TOURMALINE_AA": pins.get("TOURMALINE_AA", ""),
                                             "TOURMALINE_TEXSTREAM_MIP_BIAS": bias_used},
                             "runs": runs, "summary": summarize(runs)}
                    report["resolutions"].append(entry)
                    megapixels = res["width"] * res["height"] / 1e6
                    table.append({"resolution": res["label"],
                                  "extent": f"{res['width']}x{res['height']}",
                                  "render_scale": round(res["render_scale"], 4),
                                  "texstream_mip_bias": float(bias_used),
                                  "median_ms": entry["summary"]["median_ms"],
                                  "estimated_fps": round(entry["summary"]["estimated_fps"], 1),
                                  "ms_per_megapixel": round(entry["summary"]["median_ms"] / megapixels, 3)})
                print(json.dumps({"resolution_scaling": table,
                                  "aa": pins.get("TOURMALINE_AA", ""),
                                  "note": "asset quality pinned to the 1080p reference via "
                                          "TOURMALINE_TEXSTREAM_MIP_BIAS=log2(render_scale); "
                                          "override with --env TOURMALINE_TEXSTREAM_MIP_BIAS=... to "
                                          "measure native per-resolution quality"}, indent=2), flush=True)
                # The native-scale entry is the headline summary (compare
                # mode reads it); fall back to the last studied resolution.
                native = [e for e in report["resolutions"] if e["render_scale"] == 1.0]
                report["summary"] = (native or report["resolutions"])[-1]["summary"]
            else:
                report["summary"] = summarize(report["runs"])
        cull_mismatch = False
        if args.mode == "cull" and not failed:
            log_text = (out / "cull-01.log").read_text(encoding="utf-8", errors="replace")
            wrote = CULL_DUMP_WROTE.search(log_text)
            failed_line = CULL_DUMP_FAILED.search(log_text)
            if not wrote:
                failed = True
                report["cull_error"] = (f"engine dump failed: {failed_line.group(1)}"
                                        if failed_line else
                                        "no [CullDump] wrote line in the run log")
            else:
                dump_path = Path(wrote.group(1))
                if not dump_path.is_absolute():
                    dump_path = workdir / dump_path
                try:
                    dump = json.loads(dump_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    failed = True
                    report["cull_error"] = f"unreadable dump {dump_path}: {exc}"
                else:
                    report["cull_dump"] = dump
                    baseline = args.baseline_dir / f"{args.scenario}.json"
                    status, differs = cull_baseline_compare(dump, baseline, args.update)
                    report["cull_status"] = status
                    if differs:
                        report["cull_differs"] = differs
                    print(json.dumps({"cull": status, "scenario": args.scenario,
                                      "baseline": str(baseline),
                                      "frame": dump.get("frame"),
                                      "draw_count": dump.get("draw_count"),
                                      "visible_draws": len(dump.get("visible_draws", [])),
                                      "differs": differs}, indent=2), flush=True)
                    if status == "mismatch":
                        cull_mismatch = True
                    elif status == "missing":
                        failed = True
                        report["cull_error"] = (f"no baseline recorded for scenario "
                                                f"'{args.scenario}' at {baseline}; "
                                                f"run with --update first")
        report["ok"] = not failed
        if failed and exclusive:
            report.pop("summary", None)
        if args.mode in ("capture", "trace"):
            report["note"] = "Nsight runs are instrumented; do not compare their FPS against bench mode. Inspect the Nsight output/log for capture artifacts."
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if cull_mismatch:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
