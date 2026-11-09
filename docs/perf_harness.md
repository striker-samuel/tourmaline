
## Scenario export and replay
## Exclusive performance measurement and environment telemetry

Uninstrumented `bench` measurements must run on a quiet machine. The harness
checks system CPU, RAM and other GPU processes before priming, after priming,
and before and after **each** measured invocation. A busy or unverifiable
environment causes a nonzero exit, `"ok": false` and an `invalid_reason`
in `result.json`; **no valid benchmark summary is emitted**. Each
`environment_checks` entry includes its phase, CPU utilization, RAM
availability/usage, GPU process utilization (where supported), and reasons for
rejection. The comparison command rejects invalid benchmark reports.

Default fail thresholds are **20% system CPU**, **90% physical RAM used**,
or **5% utilization by an external GPU 3D/compute process**. These are
conservative hygiene thresholds, not a guarantee that the machine was idle
through every millisecond of the run. Browsers count whenever a browser GPU
process is doing significant 3D/compute work; an idle open browser does not
automatically invalidate results. GPU activity might also occur between
samples, so close games, browsers with GPU-heavy tabs, renderers, streaming
software, and other jobs before benchmarking.

Windows uses OS GPU Engine performance counters, Windows CPU counters, and
physical memory telemetry. Linux uses `/proc/stat`, `/proc/meminfo`,
and NVIDIA `nvidia-smi pmon` process utilization. Missing permissions,
unsupported GPU tooling (including non-NVIDIA Linux adapters), or unavailable
counters fail closed with `status: "unknown"`: do not treat those runs as
clean. Run on a supported telemetry setup for measured results. The gate
does not attempt to terminate unrelated applications.

# Agent performance harness (Benchmark + Nsight Graphics)

Tourmaline's `App` viewer and dedicated `Benchmark` executable share the same measured-frame contract: 10 internal warmup frames, a fixed benchmark timestep, and an explicit measured-frame budget. The Python standard-library harness `tools/perf_harness.py` runs this contract; it does **not** invent a second timing loop. It forces `--disable_control --bench <frames> --present immediate` for each invocation.

Requirements: Python 3.9+, a built `App` or `Benchmark`, cooked scene pak (or `.tapp`), and a GPU/driver capable of rendering it. Launch the harness from the repository root. The executable runs with its **build directory** as the working directory so staged shader and pak files resolve correctly (`--workdir` overrides this). Use `--exe` for the binary and either `--scenario` or `--pak` / `--project` for scene input. `Benchmark` has no viewer gameplay/audio extensions and refuses to start without `--bench`.


Build the dedicated CMake `Benchmark` target explicitly. It is separate from `App` and requires a cooked source; a graphical GPU session is still needed for measurements.

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5 --output out/perf/bistro
```

**The bundled `bistro-ssr-stress` entry needs a separately cooked `content/paks/bistro.pak` next to the benchmark executable.** It is not provided by the Sponza demo release. In the App viewer, load your cooked scene, select a costly view, press **TAB**, enable UI cursor with **I** if necessary, select the **Benchmarks** tab, enter a label, subsystem and optional notes, and click **Export for benchmark**. The UI derives a stable slug from the label (adding `-2`, `-3`, … to avoid collisions) and appends to tracked `benchmarks/scenarios.json`; it does not let you type an arbitrary ID. Review and commit the catalog entry.

The exporter records the cooked launch identity (`--pak` or `--project`, optionally `--scene` / `--scene-index`), camera position/direction, supported `TOURMALINE_*` renderer values, optional viewer host flags, `--with=<csv>` and `--without=<csv>` pass-graph selections, and a measured-frame budget (120 by default). The exported pass list consolidates each direction into **one** argument because the CLI retains only the last occurrence of each option. It includes SSR, SSGI, volumetric fog, contact shadows, terrain, filtering, and postprocessing where matching startup controls exist; advanced live-only sliders are **not** replayed. Benchmark runs force auto exposure off. Check the JSON for missing settings before treating two exports as equivalent. See the [scenario catalog guide](../benchmarks/README.md) for schema details and source-controlled review practices.

During a scenario replay, the harness clears inherited `TOURMALINE_*` variables, applies the catalog pins, then applies any explicit repeated `--env TOURMALINE_NAME=value` overrides. It validates the scenario ID, its source flags, the scene files relative to the binary's working directory, and basic catalog structure before launch. `--scenario` cannot be combined with `--pak` or `--project`. The `--catalog` option selects a different JSON file; the default is the tracked catalog. On Linux, use the corresponding built `Benchmark` path and executable name (without `.exe`).

## Optimized benchmarking

```powershell
python tools/perf_harness.py bench --exe out/build/x64-Release/App.exe --pak out/build/x64-Release/content/paks/bistro.pak --frames 120 --repeat 5 --output out/perf/base --env TOURMALINE_SSR=1
```

After a run begins, writes `result.json` and one `bench-NN.log` per invocation. The JSON contains the command, environment pins, raw per-run timings, median/mean/min/max frame milliseconds, and estimated FPS. Exit is nonzero if App fails, times out, or doesn't emit the expected `[perf]` line; agents must never treat empty results as success. Do not benchmark under a debugger, a capture tool, or with different shader optimization flags and compare as if the measurements were equivalent.

Use the *same* scene, camera, workload pins, GPU, and build flavor when benchmarking A/B changes; record hardware and driver information separately.

```powershell
python tools/perf_harness.py compare --baseline out/perf/base/result.json --candidate out/perf/change/result.json --max-regression 5
```

Comparison returns exit code 2 if median frame time regressed more than 5% (or the provided threshold); mismatched frame budgets, scenario IDs, scene inputs, pins, and app arguments fail closed. The statistic is the median of the per-run **average frame times**, not per-frame GPU durations. To use `compare` for A/B code changes, keep the workload configuration identical. For regression tests on CI without a GPU run `python -m unittest discover -s tools/tests`.

## Warm runs (priming)

The first invocation after any shader or pipeline change pays **pipeline compilation** and first-use driver caches inside its measured frames — observed as 6.2 ms vs 1.6 ms medians on otherwise identical content. Bench mode therefore runs one **unmeasured priming invocation per distinct environment** before the measured repeats (a 12-frame run: enough for every pass to execute and compile, cheap enough to discard; see `PRIMING_BENCH_FRAMES`). Priming entries land in `result.json` under `"priming"` with their own logs (`warmup-*.log`) and are **never summarized**; a failed priming run fails the whole bench (the measured runs never start). The disk pipeline-cache save happens at process shutdown, outside the measured window.

## Resolution scaling (`--resolutions`)

```powershell
python tools/perf_harness.py bench --scenario sponza-atrium --exe out/build/x64-Release/Benchmark.exe --resolutions 720p,1080p,1440p,4k --repeat 3 --output out/perf/scale
```

Runs one repeat-set per resolution and prints a `resolution_scaling` table: extent, render scale, median ms, estimated FPS, and **ms per megapixel** — how render time grows with the render extent. Presets: `720p 1080p 1440p 2160p 4k`; explicit 16:9 `WxH` values are accepted (the headless window is the 1920×1080 `FrameConfig` default and `TOURMALINE_RENDER_SCALE` is a single scale, so non-16:9 extents are refused, as is anything outside the engine's 0.25–2.0 clamp).

Two defaults make the study **measure pixels, not asset work**:

- **MSAA 2x** is the default AA load (`TOURMALINE_AA=msaa2x`), deliberately overriding a scenario's AA pin — the sponza-atrium canon pins `AA=off` for golden determinism, but a scaling study fixes its AA. An explicit `--env TOURMALINE_AA=...` wins.
- **Asset quality is pinned to the 1080p reference**: the texture streamer's wanted-mip demand scales with the render extent (higher resolutions legitimately demand higher-quality textures), so each resolution pins `TOURMALINE_TEXSTREAM_MIP_BIAS=log2(render_scale)` to cancel that. Override with `--env TOURMALINE_TEXSTREAM_MIP_BIAS=...` (e.g. `0`) to measure native per-resolution quality instead.

`compare` understands multires results: it compares **every** resolution and fails (exit 2) if any one regresses beyond the threshold — exactly the behavior scaling studies exist to protect. `result.json` carries per-resolution groups under `"resolutions"` (runs, per-res environment, summary); the headline `"summary"` is the native-scale (1080p) entry.

## Cull baselines (`cull` mode) — GPU-driven renderer correctness

Performance numbers cannot catch a cull chain that silently **drops visible geometry** or stops rejecting. The `cull` mode records, for a saved scenario camera, exactly which draws the GPU cull chain kept:

```powershell
# Record once (commit the baseline like scenarios.json):
python tools/perf_harness.py cull --scenario sponza-atrium --exe out/build/x64-Release/Benchmark.exe --update

# Verify after any gfx cull chain / HZB change:
python tools/perf_harness.py cull --scenario sponza-atrium --exe out/build/x64-Release/Benchmark.exe
```

The mode arms the engine's `TOURMALINE_CULL_DUMP=<frame>` pin (default frame 12; ≥ 2 — the verdict readback accepts one flight of staleness, and the pinned bench camera is static so the verdicts are identical). At that frame `GfxRenderer` reads the main view's draw-cull verdict buffer back (word *i* = 1 visible / 0 culled, per `gfxdrawcull.cs.hlsl`) and writes `exports/cull_dump_<n>.json`: the per-draw `visible_draws`/`culled_draws` index lists plus the filter's aggregate stats. The harness compares that dump against the tracked baseline (`benchmarks/cull_baselines/<scenario>.json`, one per saved camera) — **exit 0** match, **exit 2** mismatch (with the differing keys reported), **exit 1** failure/missing baseline. `--update` records a new baseline; review and commit the diff like any renderer change.

Baseline semantics: draw indices are the CPU push order (deterministic for a pinned scene + camera), verdicts are geometry/camera deterministic, and the aggregate stats pin the filter's exact work (surviving commands, triangles, cluster rejection). A deliberate cull-behavior change (a new rejection stage, a widened guard band) shows up as an explicit, reviewable baseline diff — update it **with the reasoning in the commit message**, never silently.

## Nsight GPU Trace

```powershell
python tools/perf_harness.py trace --exe out/build/x64-Release-ShaderDebug/App.exe --pak out/build/x64-Release-ShaderDebug/content/paks/bistro.pak --ngfx "C:/Program Files/NVIDIA Corporation/Nsight Graphics/host/windows-desktop-nomad-x64/ngfx.exe" --frames 120 --shader-debug --output out/perf/trace
```

This launches NVIDIA's **GPU Trace Profiler** with `--start-after-frames=10`, `--limit-to-frames=1`, and `--auto-export`. Nsight owns its trace report/export location; consult `trace-01.log` to find it. The harness records launch and exit status, **not** fabricated GPU performance counters. A trace captures GPU occupancy, scheduling, stalls, shader activity, etc. GPU Trace hardware/driver support and profiling permissions are required. See [NVIDIA GPU Trace CLI](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html).

## Nsight Graphics frame capture / shader debugging

```powershell
python tools/perf_harness.py capture --exe out/build/x64-Release-ShaderDebug/App.exe --pak out/build/x64-Release-ShaderDebug/content/paks/bistro.pak --ngfx-capture "C:/Program Files/NVIDIA Corporation/Nsight Graphics/host/windows-desktop-nomad-x64/ngfx-capture.exe" --capture-frame 20 --shader-debug --output out/perf/capture
```

`ngfx-capture` requests one frame after warmup, saves into the chosen output directory and produces the accompanying process log. Open the graphics capture in Nsight Graphics to inspect Vulkan draw/dispatch resources, bound shaders, and source correlation. See [NVIDIA Graphics Capture CLI](https://docs.nvidia.com/nsight-graphics/UserGuide/graphics-capture-cli.html). Captures and GPU Trace are *different* Nsight activities.

**Shader symbols:** Before profiling, configure a separate optimized-symbol build:

```powershell
cmake -S . -B out/build/x64-Release-ShaderDebug -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_SHADER_DEBUG=ON
cmake --build out/build/x64-Release-ShaderDebug --target App
```

This retains release `-O3` SPIR-V codegen but adds DXC `-Zi -fspv-debug=vulkan` debug tables. The CLI's `--shader-debug` **checks the build's CMakeCache**, it cannot switch debug info on at runtime. The engine additionally probes NVIDIA's device diagnostics extension for shader crash symbolication; `TOURMALINE_AFTERMATH=0` is its runtime kill switch. Some HLSL Debug builds retain optimized overrides due to known driver bugs; prefer the optimized shader-debug build for representative GPU performance.

## Agent contract and limitations

Use `--env TOURMALINE_NAME=value` repeatedly for controlled A/B pins; use `--app-arg=--no-vfs` etc. for extra App args. Once an invocation starts, the harness writes structured JSON even when the process fails or times out (argument/validation errors stop before report creation). This CLI invokes local proprietary Nsight binaries; it cannot run the GPU profiling stages on headless Linux CI without supported NVIDIA hardware. Do not compare performance reported during capture/trace against uninstrumented benchmark results. Artifact creation and source correlation must be verified by opening the captured trace; a launcher exit of zero is not proof that a valid GPU trace was exported.

The renderer's `--capture` option is specifically the RenderDoc-safe Streamline/Optick disable configuration, unrelated to Nsight's capture CLI. For Nsight, this wrapper leaves the renderer's features unchanged by default; use `--app-arg=--no-dlss` to opt out of Streamline if capture compatibility requires it.

