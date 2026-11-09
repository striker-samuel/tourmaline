# GPU Scene: meshlet-first visibility

## Status

**Wired into GPU Scene raster, pending integrator validation.** The GPU
Scene indirect command builder now prefers the existing GfxFilter cluster
(meshlet) geometry and emits an indexed command per visible meshlet. The
existing whole-submesh draw builder is a conservative fallback if required GPU
resources or the new shader are unavailable. Use
`TOURMALINE_GPU_SCENE_MESHLETS=0` to force whole-submesh GPU Scene draws.

This is indexed GPU-driven rendering using cluster-ordered indices, **not**
Vulkan task/mesh-shader dispatch. It reuses the current `gpu_scene_raster.vert`
VS, so each generated command keeps `firstInstance = original work slot`.

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
