# Agent performance harness (Benchmark + Nsight Graphics)

Tourmaline's `App` viewer and dedicated `Benchmark` executable share the same measured-frame contract: 10 internal warmup frames, a fixed benchmark timestep, and an explicit measured-frame budget. The Python standard-library harness `tools/perf_harness.py` runs this contract; it does **not** invent a second timing loop. It forces `--disable_control --bench <frames> --present immediate` for each invocation.

Requirements: Python 3.9+, a built `App` or `Benchmark`, cooked scene pak (or `.tapp`), and a GPU/driver capable of rendering it. Launch the harness from the repository root. The executable runs with its **build directory** as the working directory so staged shader and pak files resolve correctly (`--workdir` overrides this). Use `--exe` for the binary and either `--scenario` or `--pak` / `--project` for scene input. `Benchmark` has no viewer gameplay/audio extensions and refuses to start without `--bench`.

## Scenario export and replay

Build the dedicated CMake `Benchmark` target explicitly. It is separate from `App` and requires a cooked source; a graphical GPU session is still needed for measurements.

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5 --output out/perf/bistro
```

**The bundled `bistro-ssr-stress` entry needs a separately cooked `content/paks/bistro.pak` next to the benchmark executable.** It is not provided by the Sponza demo release. In the App viewer, load your cooked scene, select a costly view, press **TAB**, enable UI cursor with **I** if necessary, enter a label, subsystem and optional notes in **Benchmark scenarios**, and click **Export for benchmark**. The UI derives a stable slug from the label (adding `-2`, `-3`, … to avoid collisions) and appends to tracked `benchmarks/scenarios.json`; it does not let you type an arbitrary ID. Review and commit the catalog entry.

The exporter records the cooked launch identity (`--pak` or `--project`, optionally `--scene` / `--scene-index`), camera position/direction, supported `TOURMALINE_*` renderer values, optional viewer host flags, `--with=<csv>` and `--without=<csv>` pass-graph selections, and a measured-frame budget (120 by default). The exported pass list consolidates each direction into **one** argument because the CLI retains only the last occurrence of each option. It includes SSR, SSGI, volumetric fog, contact shadows, terrain, filtering, and postprocessing where matching startup controls exist; advanced live-only sliders are **not** replayed. Benchmark runs force auto exposure off. Check the JSON for missing settings before treating two exports as equivalent.

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

## Benchmark target reminder

The scenario-export workflow and full replay constraints are described above. This short form is useful when rerunning a committed performance baseline.

The separate `Benchmark` CMake target shares the cooked-pak host but no sample/gameplay element, and **requires `--bench`**. Build it explicitly:

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5
```

The runner reads `benchmarks/scenarios.json` from source control by default (override with `--catalog`) and resolves scene files relative to the benchmark executable's working directory. It records the active scenario pins and explicit overrides in `result.json`.

**Limit:** The replay contract covers available `RunConfig` flags, not every live-only debug-panel slider. Review the JSON before committing, and use the same cooked assets, shader flavor and hardware for A/B comparisons. See [benchmarks/README.md](../benchmarks/README.md) for catalog fields and example stress locations.
