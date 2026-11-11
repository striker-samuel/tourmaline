#!/usr/bin/env python3
"""Local-first multi-setup driver for the existing perf scenario harness."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[1]
IDENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
MODES = ("cull", "snapshot", "bench")


def resolve_exe(raw):
    """Resolve a setup executable path.

    Tolerates the extensionless spelling on Windows - the perf harness
    documents its executable as ``Benchmark[.exe]``, and the checked-in
    example matrix uses the bare spelling, which would otherwise fail
    with "executable does not exist" on win32 (case-insensitive disk,
    but the extension is required).
    """
    path = Path(raw)
    if path.exists():
        return path
    if os.name == "nt":
        with_ext = path.with_name(path.name + ".exe")
        if with_ext.exists():
            return with_ext
    return path  # absent either way: the harness reports it loudly


def load_matrix(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
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
        if "workdir" in s and not isinstance(s["workdir"], str):
            raise ValueError("invalid workdir")
        if s.get("mode", "cull") not in MODES:
            raise ValueError("invalid mode")
        if "dxc" in s and (not isinstance(s["dxc"], str) or not s["dxc"]):
            raise ValueError("dxc must be a nonempty executable path")
        pins = s.get("env", {})
        if not isinstance(pins, dict) or any(
            not isinstance(k, str) or not re.fullmatch(r"TOURMALINE_[A-Z0-9_]+", k)
            or not isinstance(v, str) for k, v in pins.items()
        ):
            raise ValueError("env requires string TOURMALINE_* pins")
    return data


def make_record(machine, setup, scenario, mode, run_id, code_id, result, rc, environment=None):
    status = "pass" if rc == 0 and result.get("ok") is True else (
        "mismatch" if mode == "cull" and rc == 2 else "error")
    if mode == "cull" and status == "pass" and result.get("cull_status") != "match":
        status = "error"
    return {"schema_version": 1, "record_id": run_id,
            "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "machine": machine, "platform": platform.platform(),
            "setup": setup, "scenario": scenario, "mode": mode, "code_id": code_id,
            "status": status, "exit_code": rc, "result": result,
            "environment": environment}


def upload(origin, token, record):
    if not origin.startswith(("http://", "https://")):
        raise ValueError("collector URL must be HTTP(S)")
    request = urllib.request.Request(
        origin.rstrip("/") + "/records", data=json.dumps(record).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 **({"Authorization": "Bearer " + token} if token else {})},
        method="POST")
    with urllib.request.urlopen(request, timeout=10) as response:
        if response.status != 201:
            raise RuntimeError("collector rejected record")


def run(args):
    matrix = load_matrix(args.matrix)
    if not IDENT.fullmatch(args.machine):
        raise ValueError("machine needs a safe short identifier")
    sys.path.insert(0, str(ROOT / "tools"))
    from perf_harness import select_scenario
    from experiment_environment import collect_environment
    for name in matrix["scenarios"]:
        select_scenario(args.catalog, name)
    failures = 0
    for setup in matrix["setups"]:
        mode = setup.get("mode", args.mode)
        # Probe once per setup: a failed probe never turns a good benchmark red.
        environment = collect_environment(Path(setup["exe"]).resolve(), setup.get("dxc"))
        for scenario in matrix["scenarios"]:
            run_id = str(uuid.uuid4())
            destination = args.output.resolve() / args.machine / setup["name"] / scenario / run_id
            destination.mkdir(parents=True, exist_ok=False)
            cmd = [sys.executable, str(ROOT / "tools/perf_harness.py"), mode,
                   "--scenario", scenario, "--catalog", str(args.catalog.resolve()),
                   "--exe", str(resolve_exe(setup["exe"]).resolve()),
                   "--output", str(destination)]
            if setup.get("workdir"):
                cmd += ["--workdir", str(Path(setup["workdir"]).resolve())]
            for k, v in sorted(setup.get("env", {}).items()):
                cmd += ["--env", k + "=" + v]
            if mode == "cull" and args.baseline_dir:
                cmd += ["--baseline-dir", str(args.baseline_dir.resolve())]
            if mode == "snapshot":
                cmd += ["--code-id", args.code_id]
            if mode == "bench":
                cmd += ["--repeat", str(args.repeat)]
            try:
                proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, timeout=args.timeout, check=False)
                rc, log = proc.returncode, proc.stdout
            except subprocess.TimeoutExpired as exc:
                rc = 124
                log = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                log += "\nscenario matrix timeout\n"
            (destination / "matrix.log").write_text(log, encoding="utf-8")
            try:
                result = json.loads((destination / "result.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                result = {"ok": False, "error": "harness produced no valid result"}
            record = make_record(args.machine, setup["name"], scenario, mode, run_id,
                                 args.code_id, result, rc, environment)
            (destination / "record.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            if args.collector:
                try:
                    upload(args.collector, args.token, record)
                except Exception as exc:
                    print("upload failed; local record retained: " + str(exc), file=sys.stderr)
                    failures += 1
            print(json.dumps({"machine": args.machine, "setup": setup["name"], "scenario": scenario,
                              "status": record["status"], "record": str(destination / "record.json")}), flush=True)
            if record["status"] != "pass":
                failures += 1
    if args.export_parquet:
        from export_experiments import export_records
        count = export_records(args.output.resolve() / args.machine,
                               args.export_parquet, "parquet")
        print(json.dumps({"parquet_export": str(args.export_parquet),
                          "records": count}), flush=True)
    return 2 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, default=ROOT / "benchmarks/scenarios.json")
    parser.add_argument("--mode", choices=MODES, default="cull")
    parser.add_argument("--machine", default=platform.node() or "local")
    parser.add_argument("--code-id", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--output", type=Path, default=Path("out/scenario-matrix"))
    parser.add_argument("--baseline-dir", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--export-parquet", type=Path,
                        help="Optional analytics export (requires extended dependencies)")
    parser.add_argument("--collector")
    parser.add_argument("--token", default=os.environ.get("TOURMALINE_RECORD_TOKEN", ""))
    args = parser.parse_args(argv)
    if args.repeat < 1 or args.timeout < 1:
        parser.error("repeat and timeout must be positive")
    try:
        return run(args)
    except (ValueError, OSError, RuntimeError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    sys.exit(main())
