# GI volume correctness and GPU performance: implementation plan

**Status:** investigation and implementation checklist (no implementation in this documentation PR)  
**Entry points:** `engine/shaders/sg_gi_volume.hlsli`, `engine/shaders/default_lit_body.hlsli`, `engine/shaders/common.hlsli`, `engine/src/Renderer/WorldRenderer.cpp`.  
**Benchmark tooling:** [perf harness](perf_harness.md), [scenario catalog](../benchmarks/README.md), [headless pins](headless_bench.md), [GI tolerance contracts](gi_volume_tolerances.md).

This document is intended to be sufficient for an implementer to pick up the work without rediscovering the profiler observations or inventing a new benchmark loop. **Fix the correctness risks first, record a reproducible baseline, then optimize and remeasure one change at a time.**

## 0. What we know, and what we do not

The original Vulkan shader profile of the lit pass showed these **dependency-attributed sample** groups:

| Profiler group | Attributed samples |
| --- | ---: |
| LSU / global-memory load | 36.87% |
| TEX / texture load | 31.29% |
| TEX / texture fetch | 15.45% |

These percentages **are not percentages of frame time** and do not prove whether the limiting resource is local-memory traffic, texture latency, occupancy, or something else. Do not add them together as a time budget or assert that removing a particular instruction will remove that percentage of GPU time.

The accompanying SPIR-V dump showed a 56-element GI descriptor array being loaded into an aggregate, logically copied into function-local storage, and later dynamically indexed. Representative patterns (IDs vary by build):

```spirv
%5429  = OpLoad %_arr_uint_uint_56 %5428 Aligned 8
%24760 = OpCopyLogical %_arr_uint_uint_56_0 %5429
OpStore %5020 %24760
%26368 = OpLoad %uint %26366
```

This supports an **array-materialization / data-movement hypothesis**, *not* proof of a hardware register spill. Confirm with optimized SPIR-V and GPU counters.

The shader also has a potentially expensive **visibility-weighted GI sampling** path. `SgGiVolumeSample` normally fetches 7 RGBA16F layers (legacy) or 9 BC6H layers (GV6H), plus a clean-cell visibility flag when visibility layers exist. When the cell is not clean, it checks eight corners, reads two visibility layers per corner, then up to 7/9 GI layers per surviving corner:

| One volume, visibility data present | Clean cell | Filtered cell, all 8 corners survive |
| --- | ---: | ---: |
| Legacy (7 layers) | 8 3D texture operations | 73 |
| BC6H (9 layers) | 10 3D texture operations | 89 |

Counts are from source, not hardware transactions; filtered cells often skip some corners. They include the first clean-flag read. Measure the *fraction of visible pixels* taking this branch before redesigning it.

## 1. Correctness gates — resolve before trusting multi-volume results

### 1.1 GI descriptor table capacity disagrees with the indexing stride

**Files:**
- `engine/include/Engine/Renderer/WorldRenderer.h`: `giVolumeLayers[56]` and `giVolumeLayersB[56]`.
- `engine/shaders/common.hlsli`: matching `uint giVolumeLayers[56]` / `giVolumeLayersB[56]`.
- `engine/src/Renderer/WorldRenderer.cpp`: writes `[v * 10u + t]`, fills or invalidates **10** slots per volume, for up to **8** volumes.
- `engine/shaders/sg_gi_volume.hlsli`: shader samples the same `vol * 10u + t` addresses.

**Problem:** 8 volumes at stride 10 require **80 slots per set**, not 56. Index `56` is first accessed while populating volume index 5 (the sixth volume). This risks out-of-bounds CPU writes, invalid shader indices, and corruption of fields after the array. This is not merely a performance issue.

**Required implementation:**
1. Introduce a 10-slot-per-volume table with a guaranteed capacity of `SG_GI_VOLUME_MAX * 10` for **both A and B**. Prefer a small separate GPU table with an address appended to `FrameUniforms`, or another layout that preserves the existing pinned offsets. Make the CPU packing and HLSL lookup share explicit constants for capacity and stride.
2. **Do not resize the two existing 56-element fields in place.** `FrameUniforms` is an ABI-locked CPU/HLSL struct with many `offsetof` assertions. Append fields, update both mirrors and size assertions, and maintain per-flight upload/lifetime safety; leave obsolete fields reserved if necessary.
3. Update **all** readers (surface GI, B-set GI, probe visualization, and any other `giVolumeLayers` consumers found with `rg`) to use the corrected table.
4. Initialize every unused slot to `UINT32_MAX`; only index valid populated slots, honoring 7-layer legacy and 9-layer BC6H formats. Validate no stale descriptors survive a disabled set or GI hot swap.
5. Add a deterministic pack/index regression test for 0, 1, 5, 6, and 8 volumes in both sets, including the last legal index (79), and run the source's CPU/HLSL layout checks.

### 1.2 B-set plane weights must read the B-set plane pool

`SgGiVolumeCompositeSetB` passes a B-set `GiVolumeGpu` to `SgGiVolumeWeight(frame, vol, pos)`, but the current `SgGiVolumeWeight` fetches planes through **`frame.giVolumes.Get().planes`**. If A/B pools differ, this incorrectly evaluates B volumes against A planes.

**Required implementation:** make the weight helper accept the correct plane-pool pointer (or provide A/B-specific wrappers) so A reads `giVolumes` and B reads `giVolumesB`. Audit all call sites and exercise a B volume whose geometry, plane order/count and fade ramps differ from A. Keep tile-mask semantics: A's mask does not cull B.

**Gate to proceed:** multi-volume A/B blending gives expected results, the 6th–8th volumes cannot overwrite unrelated frame fields, and static layout assertions pass. If test scenes exercise only <=5 volumes, still fix the capacity defect rather than assuming it is dormant.

## 2. Create a reproducible GPU test workload

**Prerequisites:** Windows/NVIDIA for Nsight stages; Python 3.9+, Ninja/CMake toolchain, a GPU-capable Release build, the relevant cooked pak or `.tapp`, and matching shaders/content in the build directory. Launch the Python harness from the **repository root**. Do not compare under an attached debugger, with GPU tracing/capture running, or across different optimization settings.

Build the dedicated benchmark target:

```powershell
cmake --build out/build/x64-Release --target Benchmark
python tools/perf_harness.py list
```

Create a **GI-heavy** saved viewpoint (the existing `bistro-ssr-stress` catalog entry targets SSR, not GI):
1. Open the **App viewer** with a scene containing the baked GI volumes and navigate to an expensive wall/doorway or overlap view.
2. Press **TAB**; use **I** to enable the UI cursor when necessary; open **Benchmark scenarios** and **Export for benchmark**.
3. Enter a label such as **GI volume stress** and `subsystem: gi` (the UI has no separate ID field). The exporter slugifies the label to `gi-volume-stress`, adding a numeric suffix when that ID already exists, and appends it to tracked `benchmarks/scenarios.json`. Review the resulting entry: it must include the actual source identity, camera position/forward, relevant `TOURMALINE_*` pins, consolidated `--with`/`--without` pass lists, and frame budget. Commit this scenario in the implementation PR or a preparatory commit.
4. Use the same cooked assets and scene throughout. The export cannot capture every live-only UI slider; pin any indispensable missing option through `RunConfig` first rather than assuming replay works.

For this investigation, use at least one **wall-adjacent/filtered** view and one **mostly clean/open** view; also add an overlapping A/B transition view if the content exposes one. A separate, controlled scenario per view is preferable to silently moving the camera.

The harness scrubs inherited `TOURMALINE_*` shell values for scenario runs. Explicit `--env TOURMALINE_NAME=value` overrides are recorded. It uses App's fixed 10 warmup frames and the scenario's measured-frame budget (or `--frames 120`).

## 3. Establish the uninstrumented baseline

Replace `gi-volume-stress` if the exporter assigned a different ID.

```powershell
python tools/perf_harness.py bench `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release/Benchmark.exe `
  --repeat 5 `
  --output out/perf/gi-baseline
```

**Retain:** `out/perf/gi-baseline/result.json` and all `bench-NN.log` files; record GPU model, driver, resolution/render scale, exact git commit, shader build flavor, cooked pak identity, scenario ID and active pins. Confirm `"ok": true`, exactly five successful measured runs, and the expected `[perf]` summaries. The statistic is the **median of per-invocation average frame times**, not the median GPU draw duration.

Run useful **diagnostic toggles** separately (these intentionally change workload/visual quality):

```powershell
# Overall surface GI-volume contribution (different environment pins).
python tools/perf_harness.py bench `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release/Benchmark.exe `
  --repeat 5 `
  --env TOURMALINE_GI_VOLUMES=0 `
  --output out/perf/gi-volumes-off

# Fog-specific GI-volume walk, if volumetric fog is active in this scene.
python tools/perf_harness.py bench `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release/Benchmark.exe `
  --repeat 5 `
  --env TOURMALINE_VOLUMETRIC_GI_VOLUMES=0 `
  --output out/perf/gi-fog-volumes-off
```

Use these toggles only to establish an *upper bound / sensitivity* on each feature's cost. **Do not** treat them as acceptable quality-preserving optimizations. The harness's `compare` mode deliberately rejects different environment pins, so inspect these results manually and label them as diagnostic.

## 4. Collect a source-correlated Nsight trace and capture

Compile the optimized-but-symbolized variant (Release C++ and shader optimizations, SPIR-V debug information retained):

```powershell
cmake -S . -B out/build/x64-Release-ShaderDebug -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_SHADER_DEBUG=ON
cmake --build out/build/x64-Release-ShaderDebug --target Benchmark
```

Ensure the same cooked content is staged for the ShaderDebug build; the benchmark host runs with that executable's **build directory as CWD**. Verify `--scenario` source paths exist there.

```powershell
python tools/perf_harness.py trace `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release-ShaderDebug/Benchmark.exe `
  --shader-debug `
  --ngfx "C:/Program Files/NVIDIA Corporation/Nsight Graphics/host/windows-desktop-nomad-x64/ngfx.exe" `
  --output out/perf/gi-trace

python tools/perf_harness.py capture `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release-ShaderDebug/Benchmark.exe `
  --shader-debug `
  --ngfx-capture "C:/Program Files/NVIDIA Corporation/Nsight Graphics/host/windows-desktop-nomad-x64/ngfx-capture.exe" `
  --capture-frame 20 `
  --output out/perf/gi-capture
```

The GPU Trace runner requests one frame after 10 warmup frames and writes `trace-01.log`; Nsight owns the actual trace/export location. The capture runner writes its own log and capture output. **Open and validate the Nsight artifacts**; process exit status alone does not prove that a usable profile was produced. Trace/capture performance is instrumented and **must not be compared as FPS** against uninstrumented `bench` runs.

Record for the lit/Main pass: GPU execution time, occupancy, registers per thread, local-memory load/store traffic, stall/dependency attribution, texture throughput/cache behavior, whether A/B-set blending is on, and the proportion of pixels following the visibility-filtered path (via a separate diagnostic if needed). Use source/assembly correlations, not just the percentage summary.

## 5. Optimization experiment order (separate changes / separate results)

### Experiment A — remove frame-array materialization

**Suspect:** `default_lit_body.hlsli` begins `mainPS` with `FrameUniforms frame = pc.frameUniforms.Get();`, and GI helpers such as `SgGiVolumeComposite` / `SgGiVolumeSample` accept `FrameUniforms` **by value**. The observed SPIR-V materializes whole 56-element arrays and dynamically indexes function-local copies.

1. Try passing a `GPU_PTR_READ(FrameUniforms)` / `vk::BufferPointer<FrameUniforms>` pointer down hot GI helpers, fetching only individual fields and per-volume descriptors. Keep necessary scalar values in a small local snapshot if beneficial. Do not introduce broad, by-value `Get()` snapshots again through a helper.
2. Propagate the change across A/B volume sampling, mask and weight helpers as required; update fog/other call sites only when they share the problematic path. Preserve logical behavior, priority order and early-out.
3. Inspect **optimized** SPIR-V before/after for `OpLoad %_arr_uint_uint_56`, `OpCopyLogical`, `OpStore` of local aggregate arrays, and dynamic `OpLoad` from function storage. Exact result IDs are not stable. A surviving array load elsewhere is not automatically a failure; prove the **hot GI chain** no longer materializes its descriptor table.
4. Record registers/thread, local-memory operations, occupancy, Main GPU time and end-to-end benchmark median. Pointer indirection is not inherently faster; reject the refactor if it gets worse.

This is the **first performance experiment** because it directly addresses supplied SPIR-V evidence.

### Experiment B — quantify the visibility-filtered slow path

**Suspect:** `sg_gi_volume.hlsli::SgGiVolumeSample`, specifically the `visB.z < 0.5` branch and up to eight corners of point-sampled GI layers.

1. Add temporary, diagnostic-only instrumentation or a shader permutation to count/visualize **clean vs filtered** pixels and, preferably, the number of contributing corners. Avoid noisy per-pixel atomics in performance measurements.
2. Compare a **diagnostic clean-path-forced** build against the original. This breaks through-wall filtering and is **not shippable**; use it solely to estimate the slow path's cost. Keep the same scene/camera/other pins.
3. If filtered cells dominate, optimize fetch count, live array ranges, reuse of visibility data and/or bake-time clean classification **without changing the authored through-wall visibility result**. Test around thin walls, doorways, volume fades and blocked corners. Do not simply relax the clean flag or remove the visibility gate.

Keep A and B paths behaviorally equivalent when blending is enabled.

### Experiment C — address register pressure / lobe handling

The surface shader accumulates nine `float3` SG amplitudes, keeps temporary per-volume/per-corner arrays, evaluates nine diffuse kernels and potentially specular SGs. Measure compiler register use, spills/local-memory and occupancy before changing representation.

Candidate scoped trials: narrow amplitude live ranges, remove unnecessary copies such as the intermediate `specAmps` array if the compiler does not already eliminate it, specialize the disabled-B-blend case, and/or evaluate only the response needed by the material. Each trial must keep diffuse/specular/fog semantics unless quality tradeoffs are explicitly approved.

### Experiment D — only if traces still justify it

Investigate volume overlap/tile-mask efficacy and texture cache efficiency. A tile mask is a **conservative** screen-space filter; do not introduce a depth rejection that drops fog samples in front of opaque surfaces. Lower-resolution GI, fewer SG lobes, or alternate bake formats are **separate quality/memory design proposals**, not default optimizations in this task.

## 6. Benchmark each candidate against the identical baseline

For each isolated, quality-preserving optimization, rebuild the **Release** benchmark, run the same scenario at least five times, archive results under a unique path, then compare:

```powershell
cmake --build out/build/x64-Release --target Benchmark

python tools/perf_harness.py bench `
  --scenario gi-volume-stress `
  --exe out/build/x64-Release/Benchmark.exe `
  --repeat 5 `
  --output out/perf/gi-candidate-A

python tools/perf_harness.py compare `
  --baseline out/perf/gi-baseline/result.json `
  --candidate out/perf/gi-candidate-A/result.json `
  --max-regression 5
```

`compare` fails on mismatched frame count, scenario, scene args, explicit environment or app args, and returns **exit code 2** when median frame time regresses by **more than** 5%. Passing does **not** prove a meaningful speedup: report the actual percent change, per-run variation, and repeat borderline results (interleave baseline/candidate measurements if thermal clocks drift). Never compare a quality-changing diagnostic run through this command. The report does not automatically prove that cooked pak bytes, GPU driver, or shader build settings match — record these yourself.

Use the same GPU and content; profile the same Main pass and do not move the camera. When improvement is below benchmark noise, report **inconclusive**, not "optimized".

## 7. Functional and visual acceptance checks

- [ ] A and B descriptor capacity/stride are consistent, including six through eight volumes; no C++ or shader out-of-bounds indexing, no legacy-vs-BC6H mismatch.
- [ ] B weights use the B plane pool; A/B blend alpha 0, intermediate and 1, different B volume layouts, and empty/missing sets behave as designed.
- [ ] CPU/HLSL `FrameUniforms` offsets and size assertions, descriptor uploads and hot-swap lifetimes are valid; verify in relevant tests.
- [ ] Clean interiors, wall-adjacent pixels, thin-wall rejection, door transitions, high/low-priority overlap and sky fallback remain visually correct.
- [ ] Fog mean sampling/visibility and probe debug sphere views still use valid descriptors and match their reference behavior.
- [ ] Images from the same saved camera/exposure show **no unexplained changes** for strictly behavior-preserving changes. Use existing capture/diff tooling or inspect differences with a documented tolerance; the intentionally broken diagnostic branch is exempt.
- [ ] `python -m unittest discover -s tools/tests` passes; run relevant compiled GI tests, shader compilation/validation, and `ctest --test-dir out/build/x64-Release --output-on-failure` where the environment supports them.
- [ ] Uninstrumented baseline/candidate `result.json` files exist and `ok=true`; attach Main-pass Nsight evidence for any claim about the root bottleneck.
- [ ] Remove temporary instrumentation, forced-fast-path toggles and any stale comments before merging.

## 8. Required implementer deliverable

Open a **code** PR (separate from this instructions-only PR) that includes:

1. A brief root-cause statement distinguishing observed SPIR-V / measured GPU counters from hypotheses.
2. Correctness fixes in logical commits, with tests for A/B planes and descriptor table bounds.
3. Separate performance commits per experiment; source and optimized SPIR-V evidence of what changed.
4. A before/after results table with scenario ID, git SHAs, GPU/driver, resolution, frame median and Main GPU duration, per-run dispersion, and percent change. Include `result.json` paths/logs and trace/capture locations.
5. Visual comparison evidence and an explicit list of any intentionally changed output semantics.

**Stop/go decision:** retain only changes that fix correctness or show repeatable performance improvement without unacceptable visual regression. If the performance hypothesis fails, keep the measurements, revert the speculative optimization, and move to the next traced bottleneck.

## References

- [Performance harness usage](perf_harness.md) and [Python runner](../tools/perf_harness.py)
- [Source-controlled benchmark scenarios](../benchmarks/README.md)
- [Headless capture and runtime pins](headless_bench.md)
- [GI volume tolerance and visibility contracts](gi_volume_tolerances.md)
- [GI surface and B-set implementation](../engine/shaders/sg_gi_volume.hlsli)
- [Lit shader entry point](../engine/shaders/default_lit_body.hlsli)
- [FrameUniforms HLSL mirror](../engine/shaders/common.hlsli)
- [FrameUniforms CPU layout](../engine/include/Engine/Renderer/WorldRenderer.h)
- [GI frame-table population](../engine/src/Renderer/WorldRenderer.cpp)
