# Source-controlled benchmark scenarios

The canonical scenario catalog is [scenarios.json](scenarios.json). It is **tracked in git**: the viewer's **TAB → Benchmark scenarios → Export for benchmark** control appends a labeled entry directly to this file. Review and commit the diff like any other renderer performance change; no hidden user profile database is involved.

Each scenario includes `id` (stable agent selector), `label` (human-friendly), `subsystem` (the stressed system, e.g. SSR, foliage, forward+, fog), `notes`, `source_args` (one or more cooked pak paths or a .tapp manifest), `camera.position`, `camera.forward`, `environment` (the replayable `TOURMALINE_*` run pins), `app_args`, and the measured `frames` budget.

Source paths in `source_args` are relative to the executable's build directory, not the repository root. The included `bistro-ssr-stress` scene is the known position in `docs/headless_bench.md`; its cooked bistro pak must exist in the build directory. The catalog must not contain machine-specific absolute paths when checked into shared source control.

## Workflow

1. Build/run the **App** viewer with a cooked pak, fly to a costly camera location, tune renderer options, and press **TAB** and **I** to enable the UI cursor (if still in FPS mode).
2. Type a descriptive **label**, **subsystem**, and optional **notes**. Press **Export for benchmark**. The UI reports the assigned unique ID (repeated labels get `-2`, etc.). Commit the resulting JSON change.
3. Build the dedicated, sample-free `Benchmark` CMake target. Use the Python harness to list and run the saved location.

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5
python tools/perf_harness.py trace --scenario bistro-ssr-stress --exe out/build/x64-Release-ShaderDebug/Benchmark.exe --shader-debug --ngfx "C:/path/to/ngfx.exe"
```

You can keep separate entries for shadow draw-call stress, alpha/overdraw, fog/volumetrics, texture streaming, and large light counts, then call each by its stable ID from an agent or CI. The runner refuses malformed/duplicate IDs, unsupported scene flags, and missing pak files before launching the executable. An extra `--env TOURMALINE_X=value` can override a saved pin for a deliberate A/B variant, which is recorded in each run's result JSON.

**Reproducibility boundary:** The export captures the camera, cooked launch identity and the renderer's supported command-line pins (AA/AO, SSR/SSGI, GI, volumetric fog, shadows, DLSS, render scale and selected postprocessing). Not every advanced live-only UI slider has an equivalent startup pin. The exporter must not pretend those unpinned controls are reproduced: add their `RunConfig` counterparts before relying on them for a benchmark. The benchmark host pins auto-exposure off to avoid run-to-run adaptation. Nsight tracing is instrumented; compare uninstrumented `bench` outputs only.
