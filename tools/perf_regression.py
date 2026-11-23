#!/usr/bin/env python3
"""Run the benchmark scenario matrix and diff it against historic results.

Local-first regression harness (stdlib only, no network):

  run      Execute every (setup, scenario) cell in a matrix JSON through
           perf_harness.py ``bench`` mode, archive the run in a datestamped
           folder under the git-ignored historic-runs directory, then (if a
           baseline exists) diff against it.
  record   Promote a candidate file to the tracked historic baseline.
  compare  Diff an existing candidate against the baseline and report
           regressions / upgrades per cell.

Every ``run`` produces a self-contained, git-ignored archive::

    out/historic-runs/                # ignored in git
      20261123_194512/                # one run, UTC datestamp
        candidate.json                # consolidated results (schema_version 1)
        summary.json                  # human-friendly index + verdicts
        results/<setup>/<scenario>/   # per-cell result.json + bench logs
        screenshots/<setup>__<scenario>.png   # one scene capture per cell
                                                 (--capture)

The baseline is a source-controlled JSON (default
``benchmarks/results/baseline.json``): commit it like any other performance
artifact so every future run diffs against the same historic numbers.

A "regression" is a cell whose median frame time grows by more than
``--max-regression`` percent (default 5%); an "upgrade" shrinks by more than
``--min-upgrade`` percent (default 3%). Cells also carry a *path* verdict
from the run log - whether the GPU-scene task/mesh draw engaged
(``[perf] task/mesh drawn tris N``) or fell back to indexed raster
(``[perf] indexed drawn tris N`` / ``[GpuSceneTaskMesh] ... indexed
fallback``) - so a silent cutover regression (task/mesh -> indexed) is
flagged even when the frame time is flat.

Screenshots (``--capture``) reuse the engine's one-shot
``TOURMALINE_CAPTURE=<frame>`` pin: the app writes
``exports/screenshot_<stamp>.png`` into its working directory, and this
harness copies the newest such file into the run archive under a stable
``<setup>__<scenario>`` name.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = ROOT / "benchmarks" / "results" / "baseline.json"
DEFAULT_RUNS_DIR = ROOT / "out" / "historic-runs"
IDENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")

# Path-engagement markers parsed out of each cell's run logs.
TASK_MESH_TRIS = re.compile(r"\[perf\] task/mesh drawn tris (\d+)")
INDEXED_TRIS = re.compile(r"\[perf\] indexed drawn tris (\d+)")
TASK_MESH_FALLBACK = re.compile(r"\[GpuSceneTaskMesh\].*indexed fallback")
INDEXED_REJECT = re.compile(r"\[GpuSceneTaskMesh\].*topology rejected \(indexed")


def load_matrix(path):
    """Read + validate a scenario matrix (schema shared with scenario_matrix.py)."""
    # utf-8-sig tolerates an editor-introduced BOM without changing normal reads.
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("expected matrix schema_version 1")
    scenarios, setups = data.get("scenarios"), data.get("setups")
    if not isinstance(scenarios, list) or not scenarios or any(
        not isinstance(s, str) or not IDENT.fullmatch(s) for s in scenarios
    ) or len(set(scenarios)) != len(scenarios):
        raise ValueError("scenarios must be unique safe IDs")
    if not isinstance(setups, list) or not setups:
        raise ValueError("setups must be a nonempty list")
    names = set()
    for s in setups:
        if not isinstance(s, dict) or not isinstance(s.get("name"), str) or not IDENT.fullmatch(s["name"]):
            raise ValueError("invalid setup name")
        if s["name"] in names:
            raise ValueError("duplicate setup name")
        names.add(s["name"])
        if not isinstance(s.get("exe"), str) or not s["exe"]:
            raise ValueError("setup needs exe path")
        if s.get("mode", "bench") not in ("bench", "cull", "snapshot"):
            raise ValueError("matrix supports bench/cull/snapshot modes")
        pins = s.get("env", {})
        if not isinstance(pins, dict) or any(
            not isinstance(k, str) or not re.fullmatch(r"TOURMALINE_[A-Z0-9_]+", k)
            or not isinstance(v, str) for k, v in pins.items()
        ):
            raise ValueError("env requires string TOURMALINE_* pins")
    return data


def resolve_exe(raw):
    """Tolerate the extensionless spelling on Windows.

    Must check ``is_file()``, not ``exists()``: on a case-insensitive Windows
    disk the extensionless "out/.../App" matches the CMake ``app/`` build
    *directory*, so ``exists()`` returns True for a directory and the resolved
    path becomes the lowercased "app" (a directory), which the harness then
    rejects as "executable does not exist".
    """
    path = Path(raw)
    if path.is_file():
        return path
    if os.name == "nt":
        with_ext = path.with_name(path.name + ".exe")
        if with_ext.is_file():
            return with_ext
    return path


def scenario_frames(catalog, scenario_id):
    """The measured frame budget for a scenario (warmup is a fixed 10)."""
    try:
        data = json.loads(Path(catalog).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return 120
    for item in data.get("scenarios", []):
        if item.get("id") == scenario_id:
            frames = item.get("frames", 120)
            return frames if isinstance(frames, int) and frames > 0 else 120
    return 120


def cell_key(setup, scenario):
    return f"{setup}|{scenario}"


def _last_int(log_text, pattern):
    values = [int(m) for m in pattern.findall(log_text)]
    return values[-1] if values else None


def parse_path_verdict(log_text):
    """Reduce a cell's run logs to a small path-engagement verdict."""
    task_mesh = _last_int(log_text, TASK_MESH_TRIS)
    indexed = _last_int(log_text, INDEXED_TRIS)
    fallback = bool(TASK_MESH_FALLBACK.search(log_text) or INDEXED_REJECT.search(log_text))
    if task_mesh is not None and task_mesh > 0:
        path = "task/mesh"
    elif indexed is not None and indexed > 0:
        path = "indexed"
    else:
        path = "unknown"
    return {"path": path, "task_mesh_tris": task_mesh, "indexed_tris": indexed,
            "fell_back_to_indexed": fallback}


def _exports_dir(setup, exe):
    """The app's screenshot exports directory: <workdir>/exports."""
    if setup.get("workdir"):
        return Path(setup["workdir"]).resolve() / "exports"
    return resolve_exe(exe).resolve().parent / "exports"


def _capture_screenshot(setup, exe, started, dest):
    """Copy the newest one-shot capture produced since ``started`` into ``dest``."""
    exports = _exports_dir(setup, exe)
    if not exports.is_dir():
        return None
    newest = None
    for png in exports.glob("screenshot_*.png"):
        try:
            if png.stat().st_mtime >= started and (newest is None or png.stat().st_mtime > newest.stat().st_mtime):
                newest = png
        except OSError:
            continue
    if newest is None:
        return None
    try:
        import shutil
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(newest, dest)
        return str(dest)
    except OSError as exc:
        print(f"[perf-regression] screenshot copy failed: {exc}", file=sys.stderr)
        return None


def run_cell(perf_harness, matrix, setup, scenario, args, cell_dir, shot_dest):
    """Run one matrix cell through perf_harness.py bench and summarize it."""
    exe = resolve_exe(args.exe or setup["exe"]).resolve()
    cmd = [sys.executable, str(perf_harness), "bench",
           "--scenario", scenario, "--catalog", str(args.catalog.resolve()),
           "--exe", str(exe), "--output", str(cell_dir)]
    if setup.get("workdir"):
        cmd += ["--workdir", str(Path(setup["workdir"]).resolve())]
    for k, v in sorted(setup.get("env", {}).items()):
        cmd += ["--env", k + "=" + v]
    if args.capture:
        # Capture on the LAST measured frame (warmup 10 + scenario frames):
        # the composite draw lands in that frame (negligible), and the ~300 ms
        # PNG export is drained at app shutdown (RunFinalizer), so neither the
        # capture nor the export leaks into the measured window.
        frame = (args.capture_frame if args.capture_frame is not None
                 else 10 + scenario_frames(args.catalog, scenario))
        cmd += ["--env", f"TOURMALINE_CAPTURE={frame}"]
    cmd += ["--repeat", str(args.repeat)]
    started = time.time()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=args.timeout, check=False)
        rc, log = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as exc:
        rc = 124
        log = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        log += "\nmatrix run timed out\n"
    except OSError as exc:
        return {"setup": setup["name"], "scenario": scenario, "status": "error",
                "error": f"cannot launch harness: {exc}"}
    (cell_dir / "matrix.log").write_text(log, encoding="utf-8")

    screenshot = None
    if args.capture and rc == 0:
        screenshot = _capture_screenshot(setup, str(exe), started, shot_dest)

    # Collect the raw engine log text (harness logs + bench logs) for the
    # path verdict.
    all_logs = []
    for log_file in sorted(cell_dir.glob("*.log")):
        try:
            all_logs.append(log_file.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    verdict = parse_path_verdict("\n".join(all_logs))

    try:
        result = json.loads((cell_dir / "result.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"setup": setup["name"], "scenario": scenario, "status": "error",
                "error": "harness produced no valid result.json",
                "exit_code": rc, "screenshot": screenshot, **verdict}

    cell = {"setup": setup["name"], "scenario": scenario,
            "status": "ok" if (rc == 0 and result.get("ok")) else "error",
            "exit_code": rc, "frames": result.get("frames"),
            "environment": result.get("environment", {}),
            "source_args": result.get("source_args"),
            "app_args": result.get("app_args"),
            "screenshot": screenshot, **verdict}
    summary = result.get("summary")
    if summary:
        cell["summary"] = summary
    if cell["status"] != "ok":
        cell["error"] = result.get("invalid_reason") or f"exit {rc}"
    return cell


def unique_run_dir(runs_dir):
    """A datestamped, collision-free archive folder for one run."""
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d_%H%M%S")
    candidate = runs_dir / stamp
    n = 1
    while candidate.exists():
        candidate = runs_dir / f"{stamp}_{n}"
        n += 1
    return candidate


def build_candidate(args, run_dir):
    matrix = load_matrix(args.matrix)
    perf_harness = ROOT / "tools" / "perf_harness.py"
    if not perf_harness.is_file():
        raise OSError(f"missing {perf_harness}")
    if not IDENT.fullmatch(args.machine):
        raise ValueError("machine needs a safe short identifier")

    cells = {}
    failures = 0
    for setup in matrix["setups"]:
        for scenario in matrix["scenarios"]:
            cell_dir = run_dir / "results" / setup["name"] / scenario
            cell_dir.mkdir(parents=True, exist_ok=True)
            shot_dest = run_dir / "screenshots" / f"{setup['name']}__{scenario}.png"
            cell = run_cell(perf_harness, matrix, setup, scenario, args, cell_dir, shot_dest)
            (cell_dir / "cell.json").write_text(json.dumps(cell, indent=2) + "\n", encoding="utf-8")
            cells[cell_key(setup["name"], scenario)] = cell
            print(json.dumps({"setup": setup["name"], "scenario": scenario,
                              "status": cell["status"], "path": cell.get("path"),
                              "screenshot": cell.get("screenshot"),
                              "median_ms": (cell.get("summary") or {}).get("median_ms")}), flush=True)
            if cell["status"] != "ok":
                failures += 1

    candidate = {"schema_version": 1, "kind": "candidate",
                 "machine": args.machine, "code_id": args.code_id,
                 "platform": platform.platform(),
                 "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                 "matrix": str(args.matrix.resolve()), "cells": cells}
    candidate_path = run_dir / "candidate.json"
    candidate_path.write_text(json.dumps(candidate, indent=2) + "\n", encoding="utf-8")

    summary = {"run_dir": str(run_dir), "machine": args.machine, "code_id": args.code_id,
               "timestamp_utc": candidate["timestamp_utc"],
               "candidate": str(candidate_path), "cells": [
                   {"setup": c["setup"], "scenario": c["scenario"], "status": c["status"],
                    "path": c.get("path"), "median_ms": (c.get("summary") or {}).get("median_ms"),
                    "screenshot": c.get("screenshot"), "error": c.get("error")}
                   for c in cells.values()]}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"[perf-regression] run archived -> {run_dir} ({len(cells)} cells, "
          f"{failures} error)", file=sys.stderr)
    return candidate, failures


def _cell_config(cell):
    return (cell.get("frames"), cell.get("environment"), cell.get("source_args"),
            cell.get("app_args"))


def compare_cells(baseline, candidate, max_regression, min_upgrade):
    """Diff candidate cells against baseline cells.

    Returns (rows, regressions, errors). Each row is a dict keyed by cell id.
    """
    base_cells, cand_cells = baseline.get("cells", {}), candidate.get("cells", {})
    rows = []
    regressions = 0
    errors = 0
    for key in sorted(set(base_cells) | set(cand_cells)):
        base, cand = base_cells.get(key), cand_cells.get(key)
        if base is None:
            rows.append({"cell": key, "verdict": "new",
                         "note": "no historic result yet"})
            continue
        if cand is None:
            rows.append({"cell": key, "verdict": "missing",
                         "note": "historic result has no candidate run"})
            errors += 1
            continue
        if cand.get("status") != "ok":
            rows.append({"cell": key, "verdict": "error",
                         "note": cand.get("error", "candidate cell failed")})
            errors += 1
            continue
        if _cell_config(base) != _cell_config(cand):
            rows.append({"cell": key, "verdict": "config-changed",
                         "note": "frames/environment/source/app args differ; "
                                 "re-record the baseline against this config"})
            errors += 1
            continue

        old = base.get("summary", {}).get("median_ms")
        new = cand.get("summary", {}).get("median_ms")
        row = {"cell": key, "baseline_ms": old, "candidate_ms": new}
        if old is None or new is None or old <= 0:
            row["verdict"] = "invalid"
            errors += 1
        else:
            change = (new / old - 1.0) * 100.0
            row["change_percent"] = round(change, 3)
            if change > max_regression:
                row["verdict"] = "regression"
                regressions += 1
            elif change < -min_upgrade:
                row["verdict"] = "upgrade"
            else:
                row["verdict"] = "unchanged"

        # Path-engagement: a silent task/mesh -> indexed cutover is a
        # regression even when frame time is flat.
        base_path, cand_path = base.get("path"), cand.get("path")
        if (base_path == "task/mesh" and cand_path not in (None, "task/mesh")
                and cand_path != base_path):
            row["path_regression"] = f"{base_path} -> {cand_path}"
            if row.get("verdict") == "unchanged":
                row["verdict"] = "regression"
                regressions += 1
        rows.append(row)
    return rows, regressions, errors


def print_compare(rows, regressions, errors, max_regression, min_upgrade):
    summary = {"thresholds": {"max_regression_percent": max_regression,
                              "min_upgrade_percent": min_upgrade},
               "regression": regressions > 0, "errors": errors,
               "regression_count": regressions, "error_count": errors,
               "rows": rows}
    print(json.dumps(summary, indent=2))
    return summary


def load_results(path, kind):
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict) or data.get("schema_version") != 1 or "cells" not in data:
        raise ValueError(f"{path} is not a schema_version 1 results file")
    return data


def latest_candidate(runs_dir):
    """The most recent archived candidate.json under the historic-runs dir."""
    if not runs_dir.is_dir():
        return None
    candidates = sorted(runs_dir.glob("*/candidate.json"))
    return candidates[-1] if candidates else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--matrix", type=Path,
                        default=ROOT / "benchmarks/scenario_matrix.example.json")
    common.add_argument("--catalog", type=Path, default=ROOT / "benchmarks/scenarios.json")
    common.add_argument("--exe", type=Path,
                        help="Override every setup's executable (e.g. out/build/x64-Release/App.exe)")
    common.add_argument("--machine", default=platform.node() or "local")
    common.add_argument("--code-id", default=os.environ.get("GITHUB_SHA", "unknown"))
    common.add_argument("--repeat", type=int, default=3)
    common.add_argument("--timeout", type=int, default=900)
    common.add_argument("--runs-dir", type=Path, default=DEFAULT_RUNS_DIR,
                        help="git-ignored base dir for datestamped run archives")
    common.add_argument("--candidate", type=Path, default=None,
                        help="candidate results file (default: latest archived run)")
    common.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    common.add_argument("--max-regression", type=float, default=5.0,
                        help="fail a cell if slower by more than this percent")
    common.add_argument("--min-upgrade", type=float, default=3.0,
                        help="report a cell as an upgrade if faster by more than this percent")

    run_p = sub.add_parser("run", parents=[common],
                           help="execute the matrix and archive it under --runs-dir")
    run_p.add_argument("--capture", action="store_true",
                       help="capture one scene screenshot per cell (TOURMALINE_CAPTURE)")
    run_p.add_argument("--capture-frame", type=int, default=None,
                       help="explicit 1-based app frame for the capture. Default: the "
                            "last measured frame (warmup 10 + scenario frames), so the "
                            "PNG export drains at shutdown and never lands in a measured "
                            "frame.")
    run_p.add_argument("--no-compare", action="store_true",
                       help="skip the post-run diff against the baseline")

    rec_p = sub.add_parser("record", parents=[common],
                           help="promote a candidate file to the historic baseline")

    cmp_p = sub.add_parser("compare", parents=[common],
                           help="diff a candidate against the baseline")

    args = parser.parse_args(argv)
    if args.repeat < 1 or args.timeout < 1:
        parser.error("repeat and timeout must be positive")

    try:
        if args.command == "run":
            run_dir = unique_run_dir(args.runs_dir.resolve())
            run_dir.mkdir(parents=True, exist_ok=False)
            candidate, failures = build_candidate(args, run_dir)
            rc = 1 if failures else 0
            if not args.no_compare and args.baseline.is_file():
                baseline = load_results(args.baseline, "baseline")
                rows, regressions, errors = compare_cells(
                    baseline, candidate, args.max_regression, args.min_upgrade)
                print_compare(rows, regressions, errors,
                              args.max_regression, args.min_upgrade)
                if regressions:
                    rc = 2
                elif errors and rc == 0:
                    rc = 1
            return rc

        if args.command in ("record", "compare"):
            candidate_path = args.candidate
            if candidate_path is None:
                candidate_path = latest_candidate(args.runs_dir.resolve())
                if candidate_path is None:
                    parser.error(f"no archived candidate under {args.runs_dir}; "
                                 "run the matrix first or pass --candidate")
            candidate_path = candidate_path.resolve()

        if args.command == "record":
            candidate = load_results(candidate_path, "candidate")
            if candidate.get("kind") not in (None, "candidate"):
                raise ValueError("record expects a candidate results file")
            if not candidate.get("cells"):
                raise ValueError("candidate has no cells; run the matrix first")
            baseline = dict(candidate)
            baseline["kind"] = "baseline"
            baseline["updated_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            args.baseline.parent.mkdir(parents=True, exist_ok=True)
            args.baseline.write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")
            print(f"[perf-regression] baseline -> {args.baseline} "
                  f"({len(baseline['cells'])} cells, source {candidate_path})", file=sys.stderr)
            print("commit this file so future runs diff against the same historic numbers",
                  file=sys.stderr)
            return 0

        if args.command == "compare":
            if not args.baseline.is_file():
                parser.error(f"baseline not found: {args.baseline}")
            if not candidate_path.is_file():
                parser.error(f"candidate not found: {candidate_path}")
            baseline = load_results(args.baseline, "baseline")
            candidate = load_results(candidate_path, "candidate")
            rows, regressions, errors = compare_cells(
                baseline, candidate, args.max_regression, args.min_upgrade)
            print_compare(rows, regressions, errors, args.max_regression, args.min_upgrade)
            return 2 if regressions else (1 if errors else 0)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    sys.exit(main())
