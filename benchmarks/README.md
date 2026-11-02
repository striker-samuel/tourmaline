# Source-controlled benchmark scenarios

The canonical scenario catalog is [scenarios.json](scenarios.json). It is **tracked in git**: the viewer's **TAB → Benchmark scenarios → Export for benchmark** control appends a labeled entry directly to this file. Review and commit the diff like any other renderer performance change; no hidden user profile database is involved.

Each scenario includes `id` (stable agent selector), `label` (human-friendly), `subsystem` (the stressed system, e.g. SSR, foliage, Forward+, fog), `notes`, `source_args` (cooked pak paths or a `.tapp` manifest, plus optional scene selector), `camera.position`, `camera.forward`, `environment` (replayable `TOURMALINE_*` startup pins), `app_args` (host and pass-graph options), and the measured `frames` budget (120 by default). The catalog uses `schema_version: 1`.

Source paths in `source_args` are relative to the executable's build directory, not the repository root. The included `bistro-ssr-stress` scene is the known position in `docs/headless_bench.md`; its cooked bistro pak must exist in the build directory. The catalog must not contain machine-specific absolute paths when checked into shared source control.

## Workflow

1. Build/run the **App** viewer with a cooked pak, fly to a costly camera location, tune renderer options, and press **TAB** and **I** to enable the UI cursor (if still in FPS mode).
2. Type a descriptive **label**, **subsystem**, and optional **notes**. Press **Export for benchmark**. The UI slugifies the label into a stable ID, appending `-2`, `-3`, etc. when needed (there is no separate ID field). Commit the resulting JSON change.
3. Build the dedicated, sample-free `Benchmark` CMake target. Use the Python harness to list and run the saved location.

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5
python tools/perf_harness.py trace --scenario bistro-ssr-stress --exe out/build/x64-Release-ShaderDebug/Benchmark.exe --shader-debug --ngfx "C:/path/to/ngfx.exe"
```

You can keep separate entries for shadow draw-call stress, alpha/overdraw, fog/volumetrics, texture streaming, and large light counts, then call each by its stable ID from an agent or local test run. The runner refuses malformed/duplicate IDs, unsupported scene flags, and missing pak files before launching the executable. An extra `--env TOURMALINE_X=value` can override a saved pin for a deliberate A/B variant, which is recorded in each run's result JSON.

**Reproducibility boundary:** The export captures the camera, cooked launch identity, viewer launch flags, startup pass-graph `--with`/`--without` lists, and supported `RunConfig` settings (AA/AO, texture filtering, SSR/SSGI, GI, volumetric fog, contact shadows, terrain, DLSS, render scale, and selected postprocessing). Not every advanced live-only UI slider has an equivalent startup pin. The exporter must not pretend those unpinned controls are reproduced: add their `RunConfig` counterparts before relying on them for a benchmark. The benchmark host pins auto-exposure off to avoid run-to-run adaptation. Nsight tracing is instrumented; compare uninstrumented `bench` outputs only.

## Validation and measurement notes

- A scenario uses either `--project <manifest.tapp>` or one or more `--pak <path.pak>` launch pairs. Optional `--scene <name>` / `--scene-index <n>` selectors are allowed. **Source paths in the catalog are relative to the benchmark binary's working directory**, typically its build directory; they are not searched relative to `benchmarks/`.
- The harness always supplies `--disable_control --bench <frames> --present immediate` and fixes its warmup to 10 frames. The dedicated `Benchmark` executable refuses runs without a positive `--bench` argument. A GPU-capable graphical session is required for real frame timings.
- For a scenario, the harness removes inherited `TOURMALINE_*` shell pins, then applies the JSON environment and explicit `--env` overrides. The runner's `compare` mode rejects differing scenario IDs, frame counts, scene args, app args, or environment pins; keep the workload identical when comparing code revisions.
- Results are written under `--output` (default `out/perf/`): `result.json` plus numbered benchmark logs. The median aggregates **per-run average frame times**. Record the executable, commit, cooked assets, GPU, driver, and resolution separately.
- The GPU-less workflow `.github/workflows/perf_scenarios.yml` runs `compileall`, Python `unittest` checks, and `perf_harness.py list`; it **does not** execute GPU benchmarks or Nsight. Locally run `python -m unittest discover -s tools/tests -v` and `python tools/perf_harness.py list` to validate the tracked catalog.

See [docs/perf_harness.md](../docs/perf_harness.md) for `bench`, `compare`, `trace` and `capture` CLI usage.
