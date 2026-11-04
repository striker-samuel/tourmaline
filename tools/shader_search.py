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


def scenario_benchmark(repo, exe, catalog, scenario, repeat, output, timeout):
    cmd = [
        sys.executable, str(repo / "tools/perf_harness.py"), "bench",
        "--exe", str(exe), "--catalog", str(catalog),
        "--scenario", scenario, "--repeat", str(repeat),
        "--output", str(output), "--timeout", str(timeout),
    ]
    result = subprocess.run(cmd, cwd=repo, capture_output=True,
                            text=True, errors="replace", timeout=timeout * (repeat + 3))
    if result.returncode:
        raise RuntimeError(f"benchmark {scenario} failed: " +
                           (result.stdout + "\n" + result.stderr)[-4000:])
    report = json.loads((output / "result.json").read_text(encoding="utf-8"))
    if report.get("mode") != "bench" or report.get("scenario") != scenario:
        raise ValueError("unexpected benchmark report")
    median = float(report["summary"]["median_ms"])
    if not math.isfinite(median) or median <= 0:
        raise ValueError(f"invalid benchmark time: {median}")
    return median


def score(measured, baseline, max_regression_percent):
    ratios = [measured[k] / baseline[k] for k in baseline]
    if any((r - 1) * 100 > max_regression_percent for r in ratios):
        return None
    return statistics.mean(math.log(r) for r in ratios)


def run_search(args):
    repo = args.repo.resolve()
    shader = (repo / args.shader).resolve()
    shader_root = (repo / "engine/shaders").resolve()
    if not shader.is_relative_to(shader_root) or not shader.is_file() or shader.suffix not in (".hlsl", ".hlsli"):
        raise ValueError("--shader must be an existing file under engine/shaders")
    exe = args.exe.resolve()
    catalog = args.catalog.resolve() if args.catalog else repo / "benchmarks/scenarios.json"
    output = args.output.resolve()
    if output.is_relative_to(repo / "engine/shaders"):
        raise ValueError("output directory must not be inside engine/shaders")
    if not exe.is_file() or not catalog.is_file():
        raise ValueError("benchmark executable and scenario catalog must exist")
    if not args.scenario:
        raise ValueError("at least one --scenario required")
    if args.iterations < 1 or args.repeat < 1 or args.timeout < 1:
        raise ValueError("iterations, repeat and timeout must be positive")
    from perf_harness import select_scenario
    for scenario in args.scenario:
        select_scenario(catalog, scenario)

    original = shader.read_bytes()
    if len(original) > args.max_source_bytes:
        raise ValueError("shader exceeds --max-source-bytes")
    source = original.decode("utf-8")
    output.mkdir(parents=True, exist_ok=True)
    api_key = os.environ.get(args.api_key_env, "") if args.api_key_env else ""
    env = os.environ.copy()
    env["SHADER_SEARCH_SHADER"] = str(shader)
    env["SHADER_SEARCH_OUTPUT"] = str(output)
    history = []
    best_source = source
    best_score = 0.0

    def build():
        ok, log = run_command(args.build_cmd, repo, env, args.timeout)
        if not ok:
            raise RuntimeError("shader build failed:\n" + log)

    def benchmarks(label):
        return {
            name: scenario_benchmark(
                repo, exe, catalog, name, args.repeat, output / label / name, args.timeout)
            for name in args.scenario
        }

    try:
        build()
        baseline = benchmarks("baseline")
        (output / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")
        for index in range(1, args.iterations + 1):
            trial = output / f"candidate-{index:03}"
            trial.mkdir(exist_ok=True)
            feedback = json.dumps(history[-6:], ensure_ascii=False)
            try:
                candidate = request_candidate(
                    args.provider, args.api_base, args.model, api_key,
                    best_source, feedback, args.timeout)
                if len(candidate.encode("utf-8")) > args.max_source_bytes:
                    raise ValueError("candidate exceeds source size limit")
                if candidate == best_source:
                    raise ValueError("model returned unchanged source")
                # Never allow another file or shell command from the model.
                (trial / "shader.hlsl").write_text(candidate, encoding="utf-8")
                shader.write_text(candidate, encoding="utf-8")
                env["SHADER_SEARCH_PHASE"] = "candidate"
                build()
                ok, log = run_command(args.validate_cmd, repo, env, args.timeout)
                if not ok:
                    raise RuntimeError("correctness gate failed: " + log)
                measured = benchmarks(f"candidate-{index:03}")
                candidate_score = score(measured, baseline, args.max_regression)
                gain = (1 - math.exp(candidate_score)) * 100 if candidate_score is not None else None
                accepted = (candidate_score is not None and
                            candidate_score < best_score and (math.exp(best_score) - math.exp(candidate_score)) * 100 >= args.min_gain)
                if accepted:
                    best_source, best_score = candidate, candidate_score
                    (output / "best.hlsl").write_text(candidate, encoding="utf-8")
                outcome = {"iteration": index, "accepted": accepted,
                           "times_ms": measured, "gain_percent": gain,
                           "reason": "accepted" if accepted else "not improved or scenario regression"}
            except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as exc:
                outcome = {"iteration": index, "accepted": False, "reason": str(exc)[-4000:]}
            finally:
                # Restore the baseline for the next trial's build unless we are
                # using the accepted candidate as the next search starting point.
                shader.write_bytes(original)
            history.append(outcome)
            (trial / "result.json").write_text(json.dumps(outcome, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(outcome), flush=True)
    finally:
        shader.write_bytes(original)
    summary = {"baseline_ms": baseline, "best_gain_percent": (1 - math.exp(best_score)) * 100,
               "best_file": str(output / "best.hlsl") if best_score < 0 else None,
               "history": history}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--shader", required=True, help="File below engine/shaders")
    p.add_argument("--exe", type=Path, required=True, help="Built Benchmark executable")
    p.add_argument("--scenario", action="append", required=True)
    p.add_argument("--catalog", type=Path)
    p.add_argument("--output", type=Path, default=Path("out/shader-search"))
    p.add_argument("--build-cmd", required=True, help="Build command (argv, not shell)")
    p.add_argument("--validate-cmd", required=True, help="Mandatory correctness command (argv, not shell)")
    p.add_argument("--provider", choices=("openai", "ollama"), default="openai")
    p.add_argument("--api-base", default=None, help="OpenAI-compatible /v1 base or Ollama server root")
    p.add_argument("--model", required=True)
    p.add_argument("--api-key-env", default="OPENAI_API_KEY", help="Name of token environment variable")
    p.add_argument("--iterations", type=int, default=5)
    p.add_argument("--repeat", type=int, default=5)
    p.add_argument("--timeout", type=int, default=600)
    p.add_argument("--max-regression", type=float, default=3.0)
    p.add_argument("--min-gain", type=float, default=0.5)
    p.add_argument("--max-source-bytes", type=int, default=200000)
    args = p.parse_args(argv)
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
