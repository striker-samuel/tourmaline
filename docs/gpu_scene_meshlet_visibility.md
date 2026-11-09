# GPU Scene: meshlet-first visibility

## Status

**Wired experimentally, but disabled by default after a measured severe regression.** The GPU
Scene indirect command builder now prefers the existing GfxFilter cluster
(meshlet) geometry and emits an indexed command per visible meshlet. The
existing whole-submesh draw builder is a conservative fallback if required GPU
resources or the new shader are unavailable. Use
`TOURMALINE_GPU_SCENE_MESHLETS=1` only for explicitly measured meshlet
experiments; unset it or set `0` for the fast whole-submesh GPU Scene path.

This is indexed GPU-driven rendering using cluster-ordered indices, **not**
Vulkan task/mesh-shader dispatch. It reuses the current `gpu_scene_raster.vert`
VS, so each generated command keeps `firstInstance = original work slot`.

## Blocking grid-overlook A/B result (2026-11-09)

All six ON and six OFF logs independently report GPU Scene active with
`admittedStatic=1751` and `legacySources=0`. KD reports
`rasterActive=1` in both variants with the temporal path engaged.

| Configuration | Reserved commands | Median frame time |
|---|---:|---:|
| Meshlets ON | 3,158,804 | 21.93 ms |
| Meshlets OFF | 7,004 | 2.51 ms |

Meshlet expansion is **8.74x slower** on this scenario. The 3,158,804
value is reserved command capacity, not the number of visible meshlets.
Fixed-size draw-build, seed, Hi-Z and history kernels dispatch across
this capacity regardless of survivor count. DO NOT MERGE OR DEFAULT-ENABLE
the existing meshlet implementation.

## Required redesign based on merged #301

The atomics-free PSO prefix shaders from #301 exist on master as
`gpu_scene_pso_prefix_count/scan/scatter.cs.hlsl`. Their own documentation
states they are *not* yet connected to the runtime, and that the
`vkCmdDrawIndexedIndirectCount` consumer and temporal pipeline conversion
remain to be implemented.

1. Derive exact current-frame meshlet candidates per GPU work item without
   multiplying the four PSO bins by a fixed meshlet lane reservation.
2. Run meshlet-frustum/cone verdicts and GPU prefix count/scan/scatter,
   producing dense surviving command arrays and GPU class counts.
3. Allocate bounded per-flight compact arrays with conservative capacity;
   detect overflow without submitting truncated geometry and preserve an
   all-or-nothing fallback.
4. Use `vkCmdDrawIndexedIndirectCount` per PSO class and migrate seed,
   Hi-Z, remainder and history to compact command counts. Instance identity
   can remain the work-slot pair, but the temporal execution should not scan
   reserved capacities.
5. Re-test the exact grid-overlook A/B, verify active GPU Scene in both
   runs and profile GPU compute/draw counters. Only reconsider default
   enablement after parity *and* improved GPU frame times.

## Pipeline

1. CPU coarse spatial selection and GPU instance frustum verdict.
2. GPU compact/LOD/submesh work selection (existing path).
3. **Meshlet-first indirect command builder** reads the current GfxFilter
   mesh/cluster tables, the persistent GPU-scene instance table, and the
   same camera-relative culling view as instance visibility.
4. Per-meshlet sphere/frustum and optional backface-cone tests reject
   individual clusters. Invalid/nonfinite cluster bounds or degenerate
   transforms conservatively keep work. Double-sided surfaces skip the cone.
5. The existing four fixed PSO bins feed depth, occluder, temporal seed,
   remainder, Main and G-buffer passes. Work identity does not change.
6. The temporal Hi-Z command gate now uses each surviving meshlet's local
   sphere, rather than an instance-wide sphere, when the new builder is active.
   The existing seed/history self-occlusion precautions remain in effect.

## Capacity and fallback

The existing four-bin indirect command ring reserves at most 1,048,576
commands per class. Meshlet lanes per work slot are computed from the
current candidate count, the registered maximum cluster count, and an upper
limit of 1,024 lanes per work slot.

If a mesh has more clusters than its reserved lane count, lane zero emits
its **entire** indexed submesh and the other lanes remain empty. This is
intentionally correct but less granular under heavy candidate loads.
Missing shaders/GPU tables or over-capacity input selects the existing
whole-submesh builder; GPU Scene admission and legacy mixed-scene fallback
remain unchanged. No cluster tail may be truncated.

Meshlet commands are not compacted. Excess empty indirect draws and
per-command raster/compute overhead may outweigh per-cluster savings.
The integrator should benchmark the large-scene and high-cluster-count
cases before keeping this default.

## Integrator checks

- DXC compile and C++ build (both Windows/MSVC and Linux/GCC); check HLSL
  push constant packing, BDA layouts and the shader package manifest.
- CTest `GpuSceneWorldTests` and GPU Scene temporal/hardware parity suites.
- Visual parity for static, mirrored/nonuniform scale, double-sided, masked,
  streamed LOD, legacy-skinned fallback, mixed GPU/legacy ownership.
- Vulkan validation, synchronization across geometry-revision changes and
  per-flight buffers, GPU crash/device-lost testing.
- Hi-Z and temporal seed/remainder disocclusion parity; check there is
  no self-occlusion or double drawing after meshlet expansion.
- Profile GPU frame time, command count and transient buffer memory under
  Sponza and replicated Sponza; compare
  `TOURMALINE_GPU_SCENE_MESHLETS=0` against enabled.

No runtime or GPU validation was run in the authoring environment.
