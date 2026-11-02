# Agent performance harness (Nsight Graphics)

Tourmaline already has a deterministic App benchmark: 10 warmup frames, fixed simulation timestep, an unthrottled present mode, and a measured-frame budget. `tools/perf_harness.py` wraps that existing contract; it does **not** invent a second timing loop.

Requirements: Python 3.9+, a built App, cooked scene pak (or .tapp), and a GPU/driver capable of running it. The harness is Python standard-library only. Launch from the repository root. App runs in the **build directory** so staged shaders/paks are found. Use `--exe` and `--pak` / `--project` to override paths.

## Optimized benchmarking

```powershell
python tools/perf_harness.py bench --exe out/build/x64-Release/App.exe --pak out/build/x64-Release/content/paks/bistro.pak --frames 120 --repeat 5 --output out/perf/base --env TOURMALINE_SSR=1
```

Writes `result.json` and one `bench-NN.log` per invocation. The JSON contains the command, environment pins, raw per-run timings, median/mean/min/max frame milliseconds, and estimated FPS. Exit is nonzero if App fails, times out, or doesn't emit the expected `[perf]` line; agents must never treat empty results as success. Do not benchmark under a debugger, a capture tool, or with different shader optimization flags and compare as if the measurements were equivalent.

Use the *same* scene, camera, workload pins, GPU, and build flavor when benchmarking A/B changes; record hardware and driver information separately.

```powershell
python tools/perf_harness.py compare --baseline out/perf/base/result.json --candidate out/perf/change/result.json --max-regression 5
```

Comparison returns exit code 2 if median frame time regressed more than 5% (or the provided threshold); mismatched frame budgets, pins, and app arguments fail closed. For regression tests on CI without a GPU run `python -m unittest discover -s tools/tests`.

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

# Verify after any gfx cull chain / HZB / GPU-scene change:
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

Use `--env TOURMALINE_NAME=value` repeatedly for controlled A/B pins; use `--app-arg=--no-vfs` etc. for extra App args. Structured JSON is saved even on benchmark failure. This CLI invokes local proprietary Nsight binaries; it cannot run the GPU profiling stages on headless Linux CI without supported NVIDIA hardware. Do not compare performance reported during capture/trace against uninstrumented benchmark results. Artifact creation and source correlation must be verified by opening the captured trace; a launcher exit of zero is not proof that a valid GPU trace was exported.

The renderer's `--capture` option is specifically the RenderDoc-safe Streamline/Optick disable configuration, unrelated to Nsight's capture CLI. For Nsight, this wrapper leaves the renderer's features unchanged by default; use `--app-arg=--no-dlss` to opt out of Streamline if capture compatibility requires it.

## Source-controlled camera scenarios / dedicated target

To export a repeatable stress-test view, launch the **App viewer** on a cooked scene and navigate to a costly camera angle. Press **TAB**, switch to UI cursor mode with **I** if needed, enter a label, subsystem and optional notes under **Benchmark scenarios**, then click **Export for benchmark**. This appends a unique ID plus the exact camera position/forward, scene launch identity and supported renderer pins to the tracked `benchmarks/scenarios.json`.

The separate `Benchmark` CMake target links the cooked-pak host but no sample/gameplay element and **requires --bench**. Build it explicitly:

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --repeat 5
```

The runner reads `benchmarks/scenarios.json` from source control by default (override with `--catalog`), resolves the cooked scene relative to the benchmark executable's working directory and applies the saved `TOURMALINE_*` pins. It clears inherited `TOURMALINE_*` values for scenario runs, so hidden developer-shell pins cannot silently change a test; explicit `--env` overrides are recorded in the result.

**Limit:** The replay contract covers available `RunConfig` flags, not every live-only debug-panel slider. Review the JSON before committing, and use the same cooked assets, shader flavor and hardware for A/B comparisons. See [benchmarks/README.md](../benchmarks/README.md) for catalog fields and example stress locations.
