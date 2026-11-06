# GPU Scene DB → instanced indirect raster: staged integration

## Scope of this PR

This PR adds `gpu_scene_instancing.h`, a **device-free reference planner**
that groups already-selected GPU Scene DB instances into compatible,
contiguous per-view ranges. It is deliberately **not enabled in the
rasterizer**: current gfx-filter multi-draw indirect and direct fallbacks are
unchanged. A separate GPU producer/shader integration is needed before any
new instance draw can be submitted safely.

The intent is to implement instanced draws **using the existing GPU Scene
DB**, not a second node-to-instance map or persistent instance registry.

## Contracts

- Input: per-view candidate list of `Handle<Instance>` (slot **and**
  generation) plus selected per-instance LOD. It does **not** perform visibility
  or LOD decisions.
- Validation: only live instance + mesh generations, complete chain bounds,
  resident/filterable selected geometry, and published LOD/submesh rows are
  admitted. Rejected candidates remain owned by the existing draw fallback.
- Batching: split on mesh slot/generation, selected LOD, geometry, material,
  submesh material, pipeline group, pass mask and instance flags. Different
  transforms may share a group.
- Output: sorted ranges and a compact, **frame-local** vector of generational
  instance references. `firstInstance` indexes that vector, never persistent
  DB storage. It is not a Vulkan command yet.
- No DB ownership changes: slot generations, dirty-row upload, topology
  reconciliation, and per-flight lifetime rules stay as implemented.

## Required before raster cutover

1. Take candidates **after** GPU instance/LOD culling, and construct a
   compacted per-flight visible-instance reference buffer. Validate the
   generation and live bit before consuming any persistent DB row.
2. Add missing per-instance raster contracts. In particular the current
   GPU Scene DB doesn't fully encode filtered draw lightmap bindings, skin
   deformation and all material/pipeline state. The planner is a conservative
   reference grouping, not proof that its groups can be rasterized together.
3. Introduce a new instanced vertex fetch contract, resolving the selected
   `SV_InstanceID`/first-instance semantics into the compacted reference,
   then the DB instance slot/generation and double-float position. The
   current `gfxfiltered.vert.hlsl` assumes one frame-local draw record
   per command and **cannot** be reused without changes.
4. Generate actual indexed indirect commands with `instanceCount > 1`
   only for compatible, visible groups, with correct index/vertex offsets
   and shader/material state; preserve GPU meshlet filtering and depth,
   motion, shadow and other passes' parity.
5. Keep existing direct/filtered fallback for evicted meshes, missing
   metadata, unsupported LOD/shader/skin variants, and stale rows. Do not
   move persistent identity into an indirect buffer.
6. A/B against the baseline candidate metrics from PR #223. Add actual
   post-cull visible count, issued command count, CPU recording time, GPU
   cull/compact time and total GPU frame timings. The reference planner
   alone does not establish a speedup.

## Validation

`tests/test_gpu_scene_world.cpp` exercises grouping across repeated
instances, material and LOD splits, stale handles, unresident LODs and
retirement. This is a headless CPU contract test. Full Vulkan runtime/CI
validation is still needed.
