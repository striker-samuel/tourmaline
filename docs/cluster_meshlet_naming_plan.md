# Clarify Cluster vs Meshlet naming

## Intent

Tourmaline currently uses **Cluster** for two unrelated concepts:

- `tour::Cluster` (`engine/include/Engine/App/cluster.h`): a scene-backed resource owner with its own lifecycle, pools, and serialization/loading boundaries.
- `tour::gfx::Cluster` (`engine/include/Engine/Renderer/gpu_scene_static.h`): a small physical geometry partition, holding triangle and vertex ranges plus bounds/cone information for indexed and task/mesh raster backends.

**Proposed change:** keep `tour::Cluster` as-is for now and rename the renderer-side `tour::gfx::Cluster` to `tour::gfx::Meshlet`. Align adjacent names, such as `ClusterConstants` → `MeshletConstants`, `kMaxClusterTris` → `kMaxMeshletTriangles`, `StaticGeometry::clusters` → `meshlets`, and `clusterStart/clusterCount` → `meshletStart/meshletCount` where those fields denote GPU geometry partitions. Rename related CPU/GPU buffer fields consistently.

This is a **terminology/clarity refactor**, not a change to geometry partitioning or scene ownership.

## Expected affected files (audit before implementation)

Core definitions and CPU registration:
- `engine/include/Engine/Renderer/gpu_scene_static.h`
- `engine/src/Renderer/gpu_scene_static.cpp`
- `engine/include/Engine/Renderer/gfx_types.h`
- `engine/src/Renderer/gfx_types.cpp`
- `engine/src/Renderer/GfxRenderer.cpp`
- `engine/include/Engine/Renderer/gpu_scene_raster_geometry.h`

Shader consumers identified by the existing cluster-range/constant names:
- `engine/shaders/gfxfilter.hlsli`
- `engine/shaders/gfxcone_debug.cs.hlsl`
- `engine/shaders/gpu_scene_meshlet_candidate_count.cs.hlsl`
- `engine/shaders/gfxskinbounds.cs.hlsl`

Potential additional callers, tests, debug views, and documentation should be found by a repository-wide search before renaming:
`gfx::Cluster`, `ClusterConstants`, `clusterStart`, `clusterCount`, `clusterBuffer`, `kMaxClusterTris`, and related shader field names. This is an initial inventory, **not** a guarantee of an exhaustive list.

## Constraints

1. Keep `tour::Cluster`, `World::clusters`, and scene/resource lifecycle APIs unchanged; these denote the other concept.
2. Preserve GPU buffer layout, alignment, size, binding slots and descriptor interfaces; `ClusterConstants` currently has a size assertion of 80 bytes.
3. Rename CPU and HLSL declarations/usages together to avoid mixed identifiers.
4. Do not silently alter cooked content, serialized formats, shader behavior, meshlet ordering, or culling semantics.
5. Validate both indexed raster and task/mesh paths, relevant shader compilation, and existing tests.

## Scope of this PR

Documentation of the intended rename and an affected-file inventory only. **No renderer or shader symbols are renamed in this PR.** The implementation should be a separate, reviewable follow-up.
