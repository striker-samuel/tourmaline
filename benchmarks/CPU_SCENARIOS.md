# CPU-only and rendered-scene benchmark scenarios

This adds two deliberately different workload modes.

## GPU-free synthetic scene scaling

`CpuBenchmark` initializes a synthetic `World` in memory and exercises the
real `World::forEachRenderItem` and the renderer's CPU-side
`MeshBoundsToWorld` / `RenderViewIntersectsSphere` helpers.
Its LOD, lightmap and draw-output stages are **synthetic approximations**,
not the production renderer's LOD resolver, lightmap residency or GPU draw
submission. No window or Vulkan device is created at runtime. It is useful
for repeatable O(N) CPU scaling studies, **not for full-frame performance
or real-scene correctness**.

```powershell
cmake --build out/build/x64-Release --target CpuBenchmark
python tools/cpu_scenario_harness.py list
python tools/cpu_scenario_harness.py run --exe out/build/x64-Release/CpuBenchmark.exe --scenario draw-items-105k --repeat 5
```

Presets in `benchmarks/cpu_scenarios.json` cover 4K, 32K, 105K and a
105K moving-camera workload. `--scenario` may be repeated; with no scenario,
the runner executes all presets. Results use `out/perf/cpu/cpu-result.json`
by default. Each run has a fixed seed, optional warmup, timing breakdown,
repeated measurements and per-metric medians.

## Real saved scene scenarios

For actual cooked Sponza/Bistro/other world scenes, the runner delegates to
the **existing** `tools/perf_harness.py bench` implementation. It retains
its scene, environment, performance hygiene and rendering contracts.
These real runs still require the Vulkan-capable `Benchmark` executable
and staged scene paks. They are CPU-oriented workloads only when the
scene is CPU-bound; they are **not GPU-free/headless CPU-only benchmarks**.

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/cpu_scenario_harness.py scene-list
python tools/cpu_scenario_harness.py scene-run --renderer-exe out/build/x64-Release/Benchmark.exe --scenario sponza-grid32-overlook --repeat 5
python tools/cpu_scenario_harness.py scene-run --renderer-exe out/build/x64-Release/Benchmark.exe --scenario sponza-grid-overlook --scenario sponza-grid32-overlook --repeat 3
```

Scene results are kept separately under
`out/perf/cpu/scenes/<scenario-id>/result.json` and include original
frame measurements, environmental checks and full host logs. A missing pak
or invalid measurement fails rather than silently reporting success.
Use `tools/perf_harness.py compare` for baseline/candidate comparisons.

No actual renderer workloads are run by the synthetic mode. No scene
catalog entry is converted into synthetic geometry without explicit user
action. These are distinct measurements and must not be compared as
equivalent CPU frame times.
