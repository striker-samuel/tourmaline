#!/usr/bin/env python3
"""CPU-only synthetic scene scenario runner (no Vulkan or GPU required)."""
import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "benchmarks" / "cpu_scenarios.json"
METRICS = ("frame_ms", "lod_ms", "bounds_ms", "lightmap_ms",
           "push_ms", "traversal_other_ms")
REQUIRED = {"id", "instances", "frames", "camera"}


def scenarios(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if data.get("schema_version") != 1 or not isinstance(data.get("scenarios"), list):
        raise ValueError("expected schema_version 1 and scenarios array")
    result = {}
    for item in data["scenarios"]:
        if not isinstance(item, dict) or not REQUIRED <= item.keys():
            raise ValueError("scenario missing required fields")
        sid = item["id"]
        if not isinstance(sid, str) or not sid or sid in result:
            raise ValueError("empty or duplicate scenario ID")
        if any(type(item[k]) is not int or item[k] < 1 or
               item[k] > (1000000 if k == "instances" else 100000)
               for k in ("instances", "frames")):
            raise ValueError("invalid count in " + sid)
        if item["camera"] not in ("fixed", "sweep"):
            raise ValueError("invalid camera mode in " + sid)
        result[sid] = item
    return result


def run(exe, scenario, warmup, repeat, timeout):
    output = []
    command = [str(exe), "--instances", str(scenario["instances"]),
               "--frames", str(scenario["frames"]),
               "--camera", scenario["camera"],
               "--seed", str(scenario.get("seed", 1)),
               "--warmup", str(warmup)]
    for _ in range(repeat):
        proc = subprocess.run(command, capture_output=True, text=True,
                              timeout=timeout, check=True)
        data = json.loads(proc.stdout.strip())
        if data.get("mode") != "cpu-only-synthetic":
            raise ValueError("executable did not report CPU-only synthetic mode")
        if any(key not in data or not isinstance(data[key], (int, float))
               or data[key] < 0 for key in METRICS):
            raise ValueError("missing or invalid CPU timing field")
        if data["frames"] != scenario["frames"] or data["instances"] != scenario["instances"]:
            raise ValueError("executable workload differs from scenario")
        output.append(data)
    return {"scenario": scenario, "command": command, "repeats": output,
            "median_ms": {key: statistics.median(x[key] for x in output)
                          for key in METRICS}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("list", "run"))
    parser.add_argument("--catalog", type=Path, default=CATALOG)
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--output", type=Path, default=ROOT / "out/perf/cpu")
    args = parser.parse_args(argv)
    try:
        catalog = scenarios(args.catalog)
        if args.action == "list":
            for sid, item in catalog.items():
                print(f"{sid:28} {item['instances']:>7} items  {item['camera']}")
            return 0
        if not args.exe or not args.exe.is_file():
            raise ValueError("--exe must point to a built CpuBenchmark executable")
        if args.repeat < 1 or args.repeat > 100 or args.warmup < 0 or args.warmup > 10000:
            raise ValueError("repeat or warmup out of range")
        if args.timeout <= 0:
            raise ValueError("timeout must be positive")
        selected = args.scenario or list(catalog)
        if len(selected) != len(set(selected)) or any(s not in catalog for s in selected):
            raise ValueError("unknown or duplicate scenario ID")
        runs = []
        for sid in selected:
            print("Running", sid, file=sys.stderr)
            runs.append({"id": sid, **run(args.exe.resolve(), catalog[sid],
                                           args.warmup, args.repeat, args.timeout)})
        report = {"schema_version": 1, "mode": "cpu-only-synthetic",
                  "executable": str(args.exe.resolve()), "runs": runs}
        args.output.mkdir(parents=True, exist_ok=True)
        target = args.output / "cpu-result.json"
        target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        for entry in runs:
            print(f"{entry['id']}: {entry['median_ms']['frame_ms']:.3f} ms/frame "
                  f"(bounds {entry['median_ms']['bounds_ms']:.3f}, "
                  f"lod {entry['median_ms']['lod_ms']:.3f})")
        print(target)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        print(f"cpu_scenario_harness: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
