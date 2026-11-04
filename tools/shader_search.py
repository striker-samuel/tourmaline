#!/usr/bin/env python3
"""Incremental, provider-agnostic HLSL optimization search (Python stdlib only).

The selected shader is temporarily replaced during builds and always restored.
Only candidates that compile, pass the caller's correctness gate, and improve
measured scenario performance are retained in the output directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
import sys
import urllib.error
import urllib.request


SYSTEM_PROMPT = """You optimize HLSL shaders for Vulkan/DXC. Return the COMPLETE
replacement source of the shader, and nothing else. Preserve bindings,
entrypoints, externally visible behavior, includes and signatures. Do not
change quality settings, skip work, or add optimization-only switches. Make
one focused optimization per response. No markdown fences or commentary."""


def normalize_source(content: str) -> str:
    content = content.strip()
    if content.startswith("```"):
        lines = content.splitlines()
        if len(lines) < 3 or not lines[-1].strip().startswith("```"):
            raise ValueError("incomplete fenced model response")
        content = "\n".join(lines[1:-1]).strip()
    if not content or "\x00" in content:
        raise ValueError("empty or invalid model output")
    return content + "\n"


def request_candidate(provider, api_base, model, api_key, shader, feedback, timeout):
    """Call OpenAI-compatible /chat/completions or native Ollama /api/chat."""
    if provider == "openai":
        url = api_base.rstrip("/") + "/chat/completions"
    else:
        url = api_base.rstrip("/") + "/api/chat"
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": (
                "Current HLSL source:\n" + shader +
                "\n\nPrior experimental feedback:\n" + feedback +
                "\n\nReturn a complete revised shader source."
            )},
        ],
        "stream": False,
    }
    if provider == "openai":
        payload["temperature"] = 0.3
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = "Bearer " + api_key
    request = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            result = json.load(response)
    except (urllib.error.URLError, ValueError) as exc:
        raise RuntimeError(f"model endpoint request failed: {exc}") from exc
    try:
        message = (result["choices"][0]["message"]["content"]
                   if provider == "openai" else result["message"]["content"])
        if isinstance(message, list):
            message = "".join(part.get("text", "") for part in message if isinstance(part, dict))
        if not isinstance(message, str):
            raise TypeError("response content is not text")
        return normalize_source(message)
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        raise RuntimeError(f"invalid model response: {exc}") from exc


def run_command(command, cwd, env, timeout):
    """Run argv without a shell; return success and a bounded diagnostic."""
    try:
        result = subprocess.run(
            shlex.split(command), cwd=cwd, env=env,
            capture_output=True, text=True, errors="replace", timeout=timeout,
            check=False)
        combined = (result.stdout + "\n" + result.stderr)[-5000:]
        return result.returncode == 0, combined
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)


def scenario_benchmark(repo, exe, catalog, scenario, repeat, output, timeout,
                       resolution=None):
    cmd = [
        sys.executable, str(repo / "tools/perf_harness.py"), "bench",
        "--exe", str(exe), "--catalog", str(catalog),
        "--scenario", scenario, "--repeat", str(repeat),
        "--output", str(output), "--timeout", str(timeout),
    ]
    if resolution:
        cmd.extend(["--resolutions", resolution])
    result = subprocess.run(cmd, cwd=repo, capture_output=True,
                            text=True, errors="replace", timeout=timeout * (repeat + 3))
    if result.returncode:
        raise RuntimeError(f"benchmark {scenario} failed: " +
                           (result.stdout + "\n" + result.stderr)[-4000:])
    report = json.loads((output / "result.json").read_text(encoding="utf-8"))
    if report.get("mode") != "bench" or report.get("scenario") != scenario:
        raise ValueError("unexpected benchmark report")
    if resolution:
        entries = report.get("resolutions") or []
        selected = [item for item in entries if item.get("label") == resolution]
        if len(selected) != 1:
            raise ValueError("benchmark did not report requested resolution " + resolution)
        dimensions = selected[0]
        expected = parse_target_resolution(resolution)
        if (dimensions.get("width"), dimensions.get("height")) != expected:
            raise ValueError("benchmark reported inconsistent target dimensions")
        summary = dimensions["summary"]
    else:
        summary = report["summary"]
    median = float(summary["median_ms"])
    if not math.isfinite(median) or median <= 0:
        raise ValueError(f"invalid benchmark time: {median}")
    return median


def score(measured, baseline, max_regression_percent):
    ratios = [measured[k] / baseline[k] for k in baseline]
    if any((r - 1) * 100 > max_regression_percent for r in ratios):
        return None
    return statistics.mean(math.log(r) for r in ratios)


def parse_target_resolution(value):
    if not value:
        return None
    from perf_harness import parse_resolutions
    entries = parse_resolutions(value)
    if len(entries) != 1:
        raise ValueError("target resolution must identify exactly one extent")
    return (entries[0]["width"], entries[0]["height"])


def compare_saved_screenshot(reference, candidate, destination, target,
                             threshold, max_fraction, max_delta):
    """Compare a freshly generated capture with saved reference pixels."""
    if not reference.is_file() or not candidate.is_file():
        raise ValueError("missing reference or freshly captured candidate screenshot")
    try:
        from pixel_compare import compare
    except ImportError as exc:
        raise RuntimeError("screenshot comparison requires Pillow (extended install)") from exc
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(candidate, destination)
    try:
        report = compare(reference, destination, threshold=threshold, stride=1,
                         diff_out=destination.with_name("difference.png"))
    except ImportError as exc:
        raise RuntimeError("screenshot comparison requires Pillow (extended install)") from exc
    if target and (report["width"], report["height"]) != target:
        raise ValueError("screenshot dimensions differ from target resolution")
    report["within_limit"] = (report["difference_fraction"] <= max_fraction and
                              report["average_channel_delta"] <= max_delta)
    return report


def run_search(args):
    repo = args.repo.resolve()
    shader = (repo / args.shader).resolve()
    shader_root = (repo / "engine/shaders").resolve()
    if not shader.is_relative_to(shader_root) or not shader.is_file() or shader.suffix not in (".hlsl", ".hlsli"):
        raise ValueError("--shader must be an existing file under engine/shaders")
    exe = args.exe.resolve() if args.exe else None
    catalog = args.catalog.resolve() if args.catalog else repo / "benchmarks/scenarios.json"
    output = args.output.resolve()
    if output.is_relative_to(repo / "engine/shaders"):
        raise ValueError("output directory must not be inside engine/shaders")
    if not args.list_regions and (not exe.is_file() or not catalog.is_file()):
        raise ValueError("benchmark executable and scenario catalog must exist")
    if not args.list_regions and not args.scenario:
        raise ValueError("at least one --scenario required")
    target = parse_target_resolution(args.target_resolution)
    screenshot_pair = bool(args.reference_screenshot) or bool(args.candidate_screenshot)
    if screenshot_pair and not (args.reference_screenshot and args.candidate_screenshot):
        raise ValueError("provide both --reference-screenshot and --candidate-screenshot")
    if args.reference_screenshot and not args.reference_screenshot.resolve().is_file():
        raise ValueError("reference screenshot does not exist")
    if args.candidate_screenshot and args.candidate_screenshot.resolve() == args.reference_screenshot.resolve():
        raise ValueError("candidate screenshot must not overwrite reference")
    if args.iterations < 1 or args.repeat < 1 or args.timeout < 1:
        raise ValueError("iterations, repeat and timeout must be positive")
    from perf_harness import select_scenario
    from experiment_environment import collect_environment
    from shader_regions import discover, preserve_identity, apply_identified_regions, load_knobs
    for scenario in args.scenario or []:
        select_scenario(catalog, scenario)

    original = shader.read_bytes()
    if len(original) > args.max_source_bytes:
        raise ValueError("shader exceeds --max-source-bytes")
    source = original.decode("utf-8")
    if args.list_regions:
        print(json.dumps({"shader": str(shader), "regions": discover(source)}, indent=2))
        return {"best_gain_percent": 0, "best_file": None}
    if output.is_dir() and any(output.iterdir()):
        raise ValueError("--output must be a new/empty directory to preserve previous experiment records")
    output.mkdir(parents=True, exist_ok=True)
    region_choices = load_knobs(args.region_knobs) if args.region_knobs else {}
    tracked_regions = discover(source)
    (output / "regions.json").write_text(
        json.dumps(tracked_regions, indent=2) + "\n", encoding="utf-8")
    api_key = os.environ.get(args.api_key_env, "") if args.api_key_env else ""
    env = os.environ.copy()
    env["SHADER_SEARCH_SHADER"] = str(shader)
    env["SHADER_SEARCH_OUTPUT"] = str(output)
    if target:
        env["SHADER_SEARCH_TARGET_WIDTH"] = str(target[0])
        env["SHADER_SEARCH_TARGET_HEIGHT"] = str(target[1])
        env["SHADER_SEARCH_TARGET_RESOLUTION"] = args.target_resolution
    environment_metadata = collect_environment(exe, args.dxc)
    (output / "environment.json").write_text(json.dumps(environment_metadata, indent=2) + "\n", encoding="utf-8")
    history = []
    best_source = source
    best_score = 0.0
    baseline_registers = None

    def build():
        ok, log = run_command(args.build_cmd, repo, env, args.timeout)
        if not ok:
            raise RuntimeError("shader build failed:\n" + log)

    def register_metrics(label):
        if not args.register_report_cmd:
            return None
        report_path = output / label / "registers.json"
        report_path.parent.mkdir(parents=True, exist_ok=True)
        env["SHADER_SEARCH_REGISTER_REPORT"] = str(report_path)
        report_path.unlink(missing_ok=True)
        ok, log = run_command(args.register_report_cmd, repo, env, args.timeout)
        if not ok or not report_path.is_file():
            raise RuntimeError("register report failed or missing: " + log)
        metrics = json.loads(report_path.read_text(encoding="utf-8"))
        if not isinstance(metrics, dict):
            raise ValueError("register report must be a JSON object")
        count = metrics.get("vgpr", metrics.get("registers_per_thread"))
        spill = metrics.get("spill_bytes", 0)
        if isinstance(count, bool) or not isinstance(count, (int, float)) or not math.isfinite(count) or count <= 0:
            raise ValueError("register report requires positive finite vgpr or registers_per_thread")
        if isinstance(spill, bool) or not isinstance(spill, (int, float)) or not math.isfinite(spill) or spill < 0:
            raise ValueError("spill_bytes must be finite and nonnegative")
        return {"registers_per_thread": count, "spill_bytes": spill, "original": metrics}

    def benchmarks(label):
        return {
            name: scenario_benchmark(
                repo, exe, catalog, name, args.repeat, output / label / name, args.timeout,
                args.target_resolution)
            for name in args.scenario
        }

    try:
        build()
        baseline_registers = register_metrics("baseline")
        baseline = benchmarks("baseline")
        (output / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")
        for index in range(1, args.iterations + 1):
            trial = output / f"candidate-{index:03}"
            trial.mkdir(exist_ok=True)
            knobs = {"register_pressure": args.register_pressure,
                     "region_overrides": region_choices}
            feedback = json.dumps({
                "target_knobs": knobs, "baseline_register_metrics": baseline_registers,
                "target_resolution": args.target_resolution,
                "visual_error_limits": {
                    "difference_fraction": args.max_difference_fraction,
                    "average_channel_delta": args.max_average_channel_delta,
                    "pixel_threshold": args.pixel_threshold,
                },
                "recent_experiments": history[-6:],
                "guidance": ("Minimize live ranges, intermediates and spills. "
                             "Unrolling can increase register pressure; don't assume faster."),
            }, ensure_ascii=False)
            visual = None
            try:
                candidate = request_candidate(
                    args.provider, args.api_base, args.model, api_key,
                    best_source, feedback, args.timeout)
                if len(candidate.encode("utf-8")) > args.max_source_bytes:
                    raise ValueError("candidate exceeds source size limit")
                source_before_hints = best_source
                identified = preserve_identity(best_source, candidate, tracked_regions)
                (trial / "regions.json").write_text(
                    json.dumps(identified, indent=2) + "\n", encoding="utf-8")
                candidate, applied_hints = apply_identified_regions(
                    candidate, region_choices, identified)
                if candidate == best_source:
                    raise ValueError("model returned unchanged source")
                # Never allow another file or shell command from the model.
                (trial / "shader.hlsl").write_text(candidate, encoding="utf-8")
                shader.write_text(candidate, encoding="utf-8")
                env["SHADER_SEARCH_PHASE"] = "candidate"
                build()
                if screenshot_pair:
                    # Prevent accidentally reusing a stale candidate image from
                    # a previous iteration, or one generated before this build.
                    args.candidate_screenshot.resolve().unlink(missing_ok=True)
                ok, log = run_command(args.validate_cmd, repo, env, args.timeout)
                if not ok:
                    raise RuntimeError("correctness gate failed: " + log)
                if screenshot_pair:
                    visual = compare_saved_screenshot(
                        args.reference_screenshot.resolve(),
                        args.candidate_screenshot.resolve(),
                        trial / "candidate.png", target,
                        args.pixel_threshold,
                        args.max_difference_fraction,
                        args.max_average_channel_delta)
                    (trial / "visual.json").write_text(
                        json.dumps(visual, indent=2) + "\n", encoding="utf-8")
                    if not visual["within_limit"]:
                        raise ValueError(
                            "visual error exceeds limit: fraction={:.6f}, avg_delta={:.6f}".format(
                                visual["difference_fraction"], visual["average_channel_delta"]))
                candidate_registers = register_metrics(f"candidate-{index:03}")
                measured = benchmarks(f"candidate-{index:03}")
                candidate_score = score(measured, baseline, args.max_regression)
                gain = (1 - math.exp(candidate_score)) * 100 if candidate_score is not None else None
                register_regression = (
                    100 * (candidate_registers["registers_per_thread"] /
                           baseline_registers["registers_per_thread"] - 1)
                    if candidate_registers is not None else None)
                registers_ok = (register_regression is None or
                                register_regression <= args.max_register_regression)
                spills_ok = (candidate_registers is None or
                             candidate_registers["spill_bytes"] <= baseline_registers["spill_bytes"])
                accepted = (registers_ok and spills_ok and candidate_score is not None and
                            candidate_score < best_score and (math.exp(best_score) - math.exp(candidate_score)) * 100 >= args.min_gain)
                if accepted:
                    best_source, best_score = candidate, candidate_score
                    tracked_regions = preserve_identity(source_before_hints, candidate, tracked_regions)
                    (output / "best.hlsl").write_text(candidate, encoding="utf-8")
                outcome = {"iteration": index, "accepted": accepted,
                           "times_ms": measured, "gain_percent": gain,
                           "knobs": knobs, "applied_hints": applied_hints,
                           "visual_error": visual,
                           "target_resolution": args.target_resolution,
                           "register_metrics": candidate_registers,
                           "register_regression_percent": register_regression,
                           "registers_ok": registers_ok, "spills_ok": spills_ok,
                           "reason": "accepted" if accepted else "not improved or scenario regression"}
            except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as exc:
                outcome = {"iteration": index, "accepted": False,
                           "knobs": knobs, "target_resolution": args.target_resolution,
                           "visual_error": visual, "reason": str(exc)[-4000:]}
            finally:
                # Restore the baseline for the next trial's build unless we are
                # using the accepted candidate as the next search starting point.
                shader.write_bytes(original)
            history.append(outcome)
            (trial / "result.json").write_text(json.dumps(outcome, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(outcome), flush=True)
    finally:
        shader.write_bytes(original)
        # Restore the compiled benchmark as well, not just the source tree.
        # Otherwise a successful search can leave stale experimental SPIR-V
        # in the build output after the HLSL is put back.
        env["SHADER_SEARCH_PHASE"] = "restore"
        build()
    summary = {"environment": environment_metadata,
               "target_resolution": args.target_resolution,
               "visual_error_limits": {"difference_fraction": args.max_difference_fraction,
                                       "average_channel_delta": args.max_average_channel_delta,
                                       "pixel_threshold": args.pixel_threshold},
               "baseline_ms": baseline,
               "baseline_registers": baseline_registers, "best_gain_percent": (1 - math.exp(best_score)) * 100,
               "best_file": str(output / "best.hlsl") if best_score < 0 else None,
               "history": history}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if args.export_parquet:
        from export_experiments import export_records
        count = export_records(output, args.export_parquet, "parquet")
        summary["parquet_export"] = {"path": str(args.export_parquet), "records": count}
    return summary


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--list-regions", action="store_true",
                   help="Discover stable shader section and loop IDs (no benchmark)")
    p.add_argument("--region-knobs", type=Path,
                   help="JSON map of independent region IDs to loop/unroll/branch/flatten hints")
    p.add_argument("--shader", required=True, help="File below engine/shaders")
    p.add_argument("--exe", type=Path, help="Built Benchmark executable")
    p.add_argument("--scenario", action="append")
    p.add_argument("--catalog", type=Path)
    p.add_argument("--output", type=Path, default=Path("out/shader-search"))
    p.add_argument("--build-cmd", help="Build command (argv, not shell)")
    p.add_argument("--validate-cmd", help="Mandatory correctness command (argv, not shell)")
    p.add_argument("--target-resolution",
                   help="Render extent: 720p/1080p/1440p/4k or 16:9 WxH")
    p.add_argument("--reference-screenshot", type=Path,
                   help="Existing trusted PNG reference screenshot")
    p.add_argument("--candidate-screenshot", type=Path,
                   help="Fresh PNG created by --validate-cmd on every iteration")
    p.add_argument("--pixel-threshold", type=int, default=6,
                   help="Sum of RGB deltas counted as differing (0..765)")
    p.add_argument("--max-difference-fraction", type=float, default=0.0,
                   help="Maximum fraction of pixels exceeding threshold")
    p.add_argument("--max-average-channel-delta", type=float, default=0.0,
                   help="Maximum mean sum of RGB absolute deltas")
    p.add_argument("--export-parquet", type=Path,
                   help="Optional analytics export (requires extended dependencies)")
    p.add_argument("--register-pressure", choices=("balanced", "minimize"), default="balanced")
    p.add_argument("--register-report-cmd",
                   help="External profiler command writing JSON to SHADER_SEARCH_REGISTER_REPORT")
    p.add_argument("--max-register-regression", type=float, default=0,
                   help="Max percent increase in VGPR/registers per thread versus baseline")
    p.add_argument("--dxc", help="Explicit DXC executable path for version collection")
    p.add_argument("--provider", choices=("openai", "ollama"), default="openai")
    p.add_argument("--api-base", default=None, help="OpenAI-compatible /v1 base or Ollama server root")
    p.add_argument("--model")
    p.add_argument("--api-key-env", default="OPENAI_API_KEY", help="Name of token environment variable")
    p.add_argument("--iterations", type=int, default=5)
    p.add_argument("--repeat", type=int, default=5)
    p.add_argument("--timeout", type=int, default=600)
    p.add_argument("--max-regression", type=float, default=3.0)
    p.add_argument("--min-gain", type=float, default=0.5)
    p.add_argument("--max-source-bytes", type=int, default=200000)
    args = p.parse_args(argv)
    if args.pixel_threshold < 0 or args.pixel_threshold > 765:
        p.error("--pixel-threshold must be in 0..765")
    if (not 0 <= args.max_difference_fraction <= 1 or
        not 0 <= args.max_average_channel_delta <= 765 or
        not math.isfinite(args.max_difference_fraction) or
        not math.isfinite(args.max_average_channel_delta)):
        p.error("visual error limits must be finite and within valid ranges")
    if (not math.isfinite(args.max_regression) or args.max_regression < 0 or
        not math.isfinite(args.max_register_regression) or
        not math.isfinite(args.min_gain) or args.min_gain < 0):
        p.error("performance limits must be finite; regression limits and min gain nonnegative")
    if args.max_register_regression < 0:
        p.error("max register regression must be nonnegative")
    if not args.list_regions and (not args.exe or not args.scenario or not args.build_cmd or not args.validate_cmd or not args.model):
        p.error("search requires --exe, --scenario, --build-cmd, --validate-cmd and --model")
    if args.api_base is None:
        args.api_base = ("http://localhost:11434" if args.provider == "ollama"
                         else "https://api.openai.com/v1")
    try:
        summary = run_search(args)
        print(json.dumps({"best_gain_percent": summary["best_gain_percent"],
                          "best_file": summary["best_file"]}, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, UnicodeError) as exc:
        print(f"shader_search: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
