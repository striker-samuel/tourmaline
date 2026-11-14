#!/usr/bin/env python3
"""Optional DuckDB/Parquet export of Tourmaline experiment records.

Only imported when explicitly requested; normal JSON capture is stdlib-only.
DuckDB writes Parquet natively, so pyarrow is not required.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


SCHEMA = """
CREATE TABLE IF NOT EXISTS experiments (
    record_id VARCHAR,
    experiment_kind VARCHAR,
    machine VARCHAR,
    setup VARCHAR,
    scenario VARCHAR,
    status VARCHAR,
    mode VARCHAR,
    code_id VARCHAR,
    candidate_id VARCHAR,
    accepted BOOLEAN,
    frame_ms DOUBLE,
    gain_percent DOUBLE,
    target_resolution VARCHAR,
    visual_difference_fraction DOUBLE,
    visual_average_channel_delta DOUBLE,
    registers_per_thread DOUBLE,
    spill_bytes DOUBLE,
    shader_sha256 VARCHAR,
    os_system VARCHAR,
    os_version VARCHAR,
    windows_build BIGINT,
    gpu_name VARCHAR,
    gpu_driver VARCHAR,
    dxc_version VARCHAR,
    dxc_sha256 VARCHAR,
    record_path VARCHAR,
    record_json VARCHAR
)
"""
COLUMNS = ("record_id", "experiment_kind", "machine", "setup", "scenario",
           "status", "mode", "code_id", "candidate_id", "accepted", "frame_ms",
           "gain_percent", "target_resolution", "visual_difference_fraction",
           "visual_average_channel_delta", "registers_per_thread", "spill_bytes",
           "shader_sha256", "os_system", "os_version", "windows_build",
           "gpu_name", "gpu_driver", "dxc_version", "dxc_sha256",
           "record_path", "record_json")


def _first_gpu(environment):
    gpu = environment.get("gpu") or {}
    adapters = gpu.get("windows_adapters") or gpu.get("nvidia_smi") or []
    return adapters[0] if adapters else {}


def _finite(value):
    import math
    if value is None:
        return None
    try:
        number = float(value)
        return number if math.isfinite(number) else None
    except (ValueError, TypeError, OverflowError):
        return None


def row_from_record(record, path, environment=None, scenario=None):
    """Normalize matrix and shader-search outcomes into one comparable row."""
    if not isinstance(record, dict):
        raise ValueError("record must be an object")
    env = environment or record.get("environment") or {}
    os_info, dxc = env.get("os") or {}, env.get("dxc") or {}
    windows = os_info.get("windows") or {}
    gpu = _first_gpu(env)
    result = record.get("result") or {}
    kind = "scenario_matrix" if "record_id" in record else "shader_search"
    if kind == "scenario_matrix":
        frame_ms = (result.get("summary") or {}).get("median_ms")
        candidate_id = None
        gain = None
        accepted = record.get("status") == "pass"
        rec_id = str(record["record_id"])
    else:
        frame_ms = (record.get("times_ms") or {}).get(scenario)
        candidate_id = "candidate-%03d" % record["iteration"]
        gain = record.get("gain_percent")
        accepted = record.get("accepted")
        rec_id = candidate_id
    visual = record.get("visual_error") or {}
    register = record.get("register_metrics") or {}
    build = {
        "record_id": rec_id,
        "experiment_kind": kind,
        "machine": record.get("machine"),
        "setup": record.get("setup"),
        "scenario": scenario or record.get("scenario"),
        "status": record.get("status") or ("accepted" if accepted else "rejected"),
        "mode": record.get("mode"),
        "code_id": record.get("code_id"),
        "candidate_id": candidate_id,
        "accepted": accepted,
        "frame_ms": _finite(frame_ms),
        "gain_percent": _finite(gain),
        "target_resolution": record.get("target_resolution"),
        "visual_difference_fraction": _finite(visual.get("difference_fraction")),
        "visual_average_channel_delta": _finite(visual.get("average_channel_delta")),
        "registers_per_thread": _finite(register.get("registers_per_thread")),
        "spill_bytes": _finite(register.get("spill_bytes")),
        "shader_sha256": record.get("candidate_sha256"),
        "os_system": os_info.get("system"),
        "os_version": os_info.get("version"),
        "windows_build": windows.get("build"),
        "gpu_name": gpu.get("name"),
        "gpu_driver": gpu.get("driver_version"),
        "dxc_version": dxc.get("version_text"),
        "dxc_sha256": dxc.get("sha256"),
        "record_path": str(path),
        "record_json": json.dumps(record, sort_keys=True),
    }
    return tuple(build[name] for name in COLUMNS)


def collect_rows(root):
    root = Path(root).resolve()
    if not root.is_dir():
        raise ValueError(f"not a directory: {root}")
    environment_path = root / "environment.json"
    environment = (json.loads(environment_path.read_text(encoding="utf-8"))
                   if environment_path.is_file() else {})
    rows = []
    # No arbitrary JSON files: these are the two known immutable record types.
    for path in sorted(root.rglob("record.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        if record.get("schema_version") == 1 and "record_id" in record:
            rows.append(row_from_record(record, path))
    for path in sorted(root.glob("candidate-*/result.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(record.get("iteration"), int):
            times = record.get("times_ms") or {}
            if isinstance(times, dict) and times:
                for scenario in sorted(times):
                    rows.append(row_from_record(record, path, environment, scenario))
            else:
                rows.append(row_from_record(record, path, environment))
    return rows


def export_records(root, destination, format="parquet"):
    """Export records only on request; fail with a useful message without DuckDB."""
    try:
        import duckdb
    except ImportError as exc:
        raise RuntimeError(
            "DuckDB optional analytics dependency is missing; "
            "run the install script with --extended / -Extended "
            "or install tools/requirements-extended.txt") from exc
    if format not in ("parquet", "duckdb"):
        raise ValueError("format must be parquet or duckdb")
    rows = collect_rows(root)
    destination = Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    db_path = str(destination) if format == "duckdb" else ":memory:"
    con = duckdb.connect(db_path)
    try:
        con.execute(SCHEMA)
        if format == "duckdb":
            # Idempotent manual exports; don't silently append duplicate records.
            con.execute("DELETE FROM experiments")
        if rows:
            placeholders = ",".join("?" for _ in COLUMNS)
            con.executemany(f"INSERT INTO experiments VALUES ({placeholders})", rows)
        if format == "parquet":
            quoted = str(destination).replace("'", "''")
            con.execute(f"COPY experiments TO '{quoted}' (FORMAT PARQUET, COMPRESSION ZSTD)")
    finally:
        con.close()
    return len(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Scenario matrix or shader search output root")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--format", choices=("parquet", "duckdb"), default="parquet")
    args = parser.parse_args(argv)
    try:
        count = export_records(args.input, args.output, args.format)
    except (RuntimeError, OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps({"exported_records": count, "path": str(args.output),
                      "format": args.format}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
