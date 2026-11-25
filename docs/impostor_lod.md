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
