# GPU Scene: per-meshlet visibility (staged integration)

This change introduces the GPU Scene **indexed meshlet command ABI** and a
conservative **per-meshlet frustum** compute kernel. It is a foundation, **not
a runtime cutover**: the current GPU Scene raster builder still issues
whole-submesh indexed indirect commands. No default renderer behavior changes.

## Input and command contract

- Source clusters are the existing `gfx::Cluster` registrations, whose
  triangle ranges address the cluster-ordered index heap. No second meshlet
  cooker is required.
- `ResolveRasterMeshlet` generates `(firstIndex, indexCount, workSlot)`
  records only for valid clusters. It cannot point at a different mesh.
- The compute kernel validates live instance slot/generation before emitting a
  draw. Every candidate writes a complete 20-byte indexed-indirect command.
- A visible cluster keeps `firstInstance = workSlot`, so the existing
  GPU Scene indexed VS can resolve the correct material and transform.
- Bounding spheres are interpreted in cluster-local coordinates, transformed
  by the live instance basis and rebased from DoubleFloat against the
  visibility view origin before six-plane frustum testing.
- Invalid/missing/unbounded spheres and invalid view inputs fail **open**.
  Invalid instance identities or broken work references fail **closed**.

## Remaining work before enabling

1. Publish a flight-safe, generation/revision-validated candidate stream from
   persistent indexed geometry descriptors and the GPU-produced work records.
   The current command builder expands one draw per submesh; it must instead
   expand one candidate per cluster, with capacity and overflow checks.
2. Build the meshlet culling view from the same relative planes/origin as the
   existing GPU Scene instance visibility path and dispatch the kernel in the
   render graph. Add explicit upload/read and indirect read barriers.
3. Allocate and group commands by the same four pipeline classes used by
   raster Main/depth. Preserve work-slot identity and masked/double-sided
   admission. Keep legacy/submesh fallback on publication failure.
4. Integrate the cluster results with the temporal seed/remainder, Hi-Z
   occluder, Main, G-buffer/depth and visibility history passes. A cluster
   must not be culled against its own seed depth.
5. Add conservative cone backface culling (including mirrored/nonuniform
   affine transforms) and per-meshlet Hi-Z culling, with masked/double-sided
   and non-finite fail-open behavior.
6. Run shader compilation, C++ tests, GPU parity (static, skinned fallback,
   masked and streamed LOD), Vulkan validation, and performance captures.

Until those steps land, this branch does **not** enable per-meshlet visibility
in production. The new shader is not wired into the engine shader manifest or
the renderer's pipeline/dispatch path.
