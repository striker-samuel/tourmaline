# Optional experiment analytics

Tourmaline's benchmark and shader search tools remain **stdlib-only by
default**. All raw `result.json`, `record.json`, logs, screenshots and trace
files remain the source of truth. DuckDB and Parquet are optional **analysis
outputs**, not another tracing stack. Tourmaline continues using Optick and
Nsight Graphics for detailed CPU/GPU tracing.

## Installation profiles

| Profile | Windows | Linux | Adds |
| --- | --- | --- | --- |
| Standard (default) | `./install_dependencies.ps1` | `./install_dependencies.sh` | Existing C++ and engine build dependencies only |
| Extended (opt-in) | `./install_dependencies.ps1 -Extended` | `./install_dependencies.sh --extended` | Standard + Python `duckdb` and `Pillow` |

Extended runs `python -m pip install -r tools/requirements-extended.txt` in
the **active Python environment**. Use a virtual environment to keep global
Python packages untouched. Python, pip and internet/package index access
must be available; failures are reported as install failures. Extended
dependencies are **never** installed during a standard install.

DuckDB has built-in Parquet support. No PyArrow, separate Parquet package,
database service or Grafana stack is necessary. Pillow is optional but useful
for the existing screenshot comparison script.

## Export experiment records

```sh
# Export collected scenario records to Parquet
python tools/export_experiments.py --input out/scenario-matrix --output out/analytics/scenarios.parquet

# Or directly into a portable DuckDB database
python tools/export_experiments.py --input out/scenario-matrix --output out/analytics/scenarios.duckdb --format duckdb

# Opt-in export at end of a scenario matrix run
python tools/scenario_matrix.py --matrix benchmarks/scenario_matrix.example.json --export-parquet out/analytics/local.parquet

# Opt-in export at end of shader search (add to other required flags)
python tools/shader_search.py --help
# ... --export-parquet out/analytics/shader-search.parquet
```

The exporter reads `record.json` from the scenario matrix and
`candidate-*/result.json` from shader-search outputs, adding available
machine/OS, GPU driver and DXC fields as queryable columns. It also retains
the complete JSON record in `record_json` to avoid losing unusual fields.
Native `.opt` and Nsight capture files are not translated to Parquet; refer
to and investigate them using their existing profiler tools.

These are **on-demand snapshots**: exporting again replaces a Parquet file,
and a DuckDB export refreshes its experiments table. For accumulated
multi-machine analysis, export the containing record directory rather than
individual result folders. Ensure records share compatible benchmark scenes,
resolution, rendering settings and driver/compiler provenance before comparing
timings.

Without the optional `duckdb` package, regular scenario runs and shader search
work unchanged. An explicitly requested export fails with installation
instructions instead of silently producing an incomplete file.
