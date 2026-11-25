# Impostor LOD (experimental foundation)

The depth baker in Engine/Asset/impostor_depth_bake.h is a CPU reference for opaque static indexed geometry. It produces object-local orthographic depth and coverage and reconstructs indexed triangle patches without bridging missing samples or large depth discontinuities.

## Intended integration (not implemented yet)

- Derive octahedral multi-view albedo, normal, coverage and depth at import time, before source geometry is discarded. Include settings in the derive fingerprint and keep assets associated with the base mesh identity.
- Append an impostor representation to the existing cooked sibling chain, with conservative mesh-local error, complete bounds, residency and hysteresis. Do not publish filterable geometry IDs until registration succeeds. Fall back to the last available mesh.
- Raster impostors need a dedicated material and depth-correct shader shared by main and depth passes. Do not substitute untextured planes for authored material.
- The expensive RT option should reconstruct and combine several depth views into a static triangle proxy, upload into the existing position/index heaps and build one shared BLAS per proxy. Select the TLAS instance representation separately from camera-facing raster views; off-screen shadow casters must not disappear. Preserve full geometry fallback for alpha-masked or unsupported assets.
- The current ray-query path handles sun visibility; reflection support requires a separate hit shading implementation.

## Validation

The reference baker is not connected to the importer, pak, renderer or Vulkan builder yet. Test nearest-surface selection, empty coverage, discontinuities, invalid indices, deterministic output, LOD streaming and fallback, and GPU validation before enabling this feature. Compare silhouettes and shadow error against LOD0 and measure BLAS size/build and traversal time.

## Integrator acceptance gates

Do not enable by default until all gates pass. Record hardware, driver, build configuration, scene, camera path and capture settings with results.

| Area | Acceptance |
|---|---|
| CPU baking | Repeat identical input twice; atlas coverage/depth and indices match. Verify six axis views, nearest surface, negative depth, degenerate/invalid input and discontinuity rejection. |
| Asset cooking | Opt-in settings change derive fingerprint; recook is idempotent; package round trip retains view transforms, channel formats, bounds, and mesh identity. Legacy packages load unchanged. |
| Raster | Compare against LOD0 on a full orbit, including tile boundaries; no opaque background, atlas bleeding, NaNs, or inconsistent main/depth silhouettes. Verify relighting and depth occlusion. |
| LOD | Exercise every mesh-to-impostor boundary with hysteresis, F5/debug overrides, scaled and mirrored instances, all supported GPU scene backends, and invalid/missing atlas fallback. |
| Streaming | Force eviction and promotion under a constrained budget; no stale descriptors, invalid geometry IDs, missing draws or in-flight use-after-free. |
| RT | Run Vulkan validation on KHR ray-query hardware; verify shared BLAS per proxy, distinct TLAS transforms, off-screen occluders, negative/nonuniform scale and reload. Unsupported hardware and masked inputs retain existing fallback. |
| Performance | Capture near/medium/far camera sweeps; report frame GPU time, atlas and geometry bytes, BLAS bytes, build time and ray traversal time for mesh, raster impostor and RT proxy modes. |
| Regression | Run GpuSceneLodTests, GpuSceneWorldTests, GpuSceneDatabaseTests, HeaderSmoke, RendergraphContractTests and text integrity. Compile all affected HLSL variants with DXC. |

Suggested provisional visual gate: on an opaque static reference asset, at the intended switch distance compare silhouettes against LOD0 with a one-pixel tolerance; report coverage IoU and depth-error percentiles. Use scene-specific budgets for thin geometry; do not claim the six-view proxy is watertight. Integrator must approve thresholds and record failures before enabling the feature.

The current six-view CPU atlas stores only depth and coverage (3x2 layout); albedo/normal baking, serialization, runtime selection, GPU upload and BLAS integration remain unimplemented. The atlas reconstruction combines independent view patches, not a closed surface.
