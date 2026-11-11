# GPUScene hierarchical geometry LOD: phase 1 reference selector

This PR adds a device-free HLOD node ABI, validation and selection oracle.
It does **not** hook raster ownership, change the existing GPUScene LOD
shader, or claim a complete production HLOD implementation.

## Selection invariants
- Single rooted forward-only tree; contiguous child ranges, no repeated
  children/orphans, max depth 64, monotonically nonincreasing geometric error.
- Mesh-local conservative bounds/error; transform the camera to mesh local
  coordinates and pass a conservative instance scale.
- Select a resident parent when its projected error is within pixel budget;
  otherwise descend; if any descendant is unavailable, roll back the complete
  child result and use a resident parent proxy.
- No resident covering proxy => MissingGeometry and no partial output.
- Capacity and invalid topology/view also produce no output.
- Logical geometry IDs must later resolve to flight-safe physical pages.

## Remaining production integration
1. Cook parent proxy meshes and a hierarchy with verified geometric coverage.
2. Publish generational node records and per-flight geometry-page residency.
3. Add compute traversal between GPUScene instance culling and submesh work
   generation, producing selected meshlet jobs with material/PSO identity.
4. Bind selected jobs to existing GPUScene task/mesh raster and indexed
   fallback; preserve no-double-draw ownership and temporal/Hi-Z behavior.
5. Add real Vulkan screenshots, missing-page tests, sparse refinement,
   shadow-view tests, synchronization and GPU perf comparisons.

The CPU tests are a reference oracle only, not proof of GPU parity.
