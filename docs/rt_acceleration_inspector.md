# Hardware TLAS / BLAS inspector (experimental)

In **Rendering > Lighting**, enable **Hardware ray-traced sun shadows (KHR ray query)** and expand **TLAS / BLAS inspector**.

The inspector displays:

- Whether the device supports ray queries and whether the **actual GPU TLAS** is ready.
- Cached CPU **candidate** TLAS instance count, rejected candidates and unique BLAS geometry count.
- Total instanced triangles versus unique BLAS triangles (a quick sharing sanity check).
- First 256 BLAS candidates with triangle counts, instance references and alpha MASK status.
- First 256 TLAS candidates with their BLAS index, triangle count and translation.

The initial version is a **hierarchical data inspector**, not a 3D wireframe bounding-box overlay, a Vulkan BVH node viewer, or a ray-hit heatmap.

## Performance and accuracy

The engine computes the CPU inspection snapshot only when the renderer's ray-tracing caster list changes. The collapsed panel performs no traversal of candidates. Expanding it only presents cached metadata: **no GPU readback, no additional ray tracing, no Vulkan synchronization, and no extra render passes**.

BLAS grouping uses the same mesh offsets, vertex count and opacity-material key as the Vulkan builder. Counts represent *input candidates*, not verified resident GPU allocations; the builder additionally rejects invalid buffer ranges. The readiness indicator is authoritative for GPU TLAS availability. BLAS memory usage, internal BVH node counts, compaction, update/refit times and per-ray costs are **not** exposed or estimated.

The existing ray-query path itself still rebuilds synchronously when the world or caster transforms change. This inspector does not make builds asynchronous or eliminate that pre-existing stall. On unsupported devices, ray shadows stay on the raster fallback.

## Validation

The `SdfTests` target now also tests RT debug candidate rejection, BLAS sharing across transforms, opacity-material splits, source indices, and unique/instanced triangle totals. This is a CPU-only contract test; it does not validate a Vulkan BLAS/TLAS build.

After building on a ray-query-capable GPU:
1. Enable hardware sun shadows; expand the inspector and confirm the GPU TLAS reports **ready**.
2. Duplicate a mesh with a different transform; TLAS instances should increase while unique BLAS count should stay constant.
3. Replace one mesh or change its opacity mode; confirm BLAS grouping changes.
4. Remove the casters; confirm no stale GPU TLAS is reported as ready.
5. With the panel closed, compare frame time to baseline; the debug snapshot should only refresh when caster inputs change.

No native C++ compilation or Vulkan run was performed during authoring; CI and hardware testing are required.
