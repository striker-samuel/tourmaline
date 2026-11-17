# Proposal: pak-precounted linear PSO bins per frame flight

## Goal

Move material/PSO draw classification sizing out of the frame hot path.
Enumerate all *possible* draw candidates (including authored LOD surfaces)
when pak metadata is available, and reserve one contiguous per-flight region.
Keep Tourmaline's existing eight indexed-raster PSO classes from
`gpu_scene_raster.h`; skinning is a property of geometry, not a ninth class.

This is an **additive staging PR**. It does not change live Main ownership,
the GPU-driven prefix/scatter compaction, or the legacy skinned fallback.
In particular it does **not** claim to fix the knight's frame-1 Main device
fault. That fault needs an independent synchronization/mesh shader diagnosis.

## Contract delivered by this PR

- `PakDrawClass`: class plus stable source/candidate identity.
- `PrecountPakPsoBins`: validate class IDs; count each class; prefix-sum
  class capacities; publish only a complete valid plan.
- `FrameLinearSceneDesc`: trivially copyable, pointer-free metadata with
  eight contiguous PSO ranges and optional instance/skin ranges.
- `PackFramePsoBins`: no per-bin allocations, stable counting scatter into
  caller-provided contiguous memory; bound checks **before writes**.
- `ValidFramePsoBins`: verify contiguous active ranges, count and capacity.
- CPU-only regression test; no GPU or pak files necessary.

The frame packer takes a caller-owned span deliberately. A caller may allocate
it from an acquired flight's bump arena, but **must not use FrameScratch**
if any graph recording callback or asynchronous uploader keeps the pointer
past the reset boundary.

## Follow-up integration (not included here)

1. At pak scene metadata enumeration / cooking, build candidate descriptors
   for every potentially drawable (mesh, LOD, surface, material variant).
   Count **draw candidates**, not only mesh assets; one mesh may generate
   multiple classifiable draws. Persist the class histogram or reconstruct
   it from metadata before scheduling GPU uploads.
2. When the live world snapshot changes, classify resident/eligible records.
   Do not confuse the full pak *capacity* with active draw *count*. An
   authored LOD can have a distinct PSO class. Handle runtime procedural
   content and override providers by re-planning or a checked capacity grow.
3. After acquiring/retiring a flight, reserve one aligned arena block with
   checked size arithmetic for the header, class offsets, draw indices,
   instances and skin ranges. Define byte offsets for typed arrays if sharing
   one allocation; the current header uses **element offsets** into named
   arrays. Fill and publish the snapshot atomically.
4. Swap readers individually after parity testing: legacy Main,
   depth/prepass, GPU indexed raster, task-mesh work, motion and shadows.
   Preserve the same flight generation and retirement barriers.
5. Verify that a failed classification or GPU buffer allocation leaves
   previous published data intact and takes the existing fallback path
   without admitting partial skinned draws.

## Invariants

- One possible draw contributes to exactly one of the eight class capacities.
- Every active draw maps to an authorized class and has a unique position
  within its class range; classification order stays stable.
- Class ranges are adjacent with no gaps; counts never exceed capacity.
- No pointer to retired-frame scratch escapes into asynchronous recording.
- Existing GPU visibility/culling still determines actual indirect counts:
  the CPU pak precount is a **reservation upper bound**, not a substitute
  for GPU occlusion or LOD selection.
- GPU upload buffers and PSO handles remain fence-safe persistent resources.
- Snapshot version/generation changes when published source metadata changes.

## Testing and rollout

Run `PakLinearPsoBinsTests` (CTest name `pak_linear_pso_bins_tests`).
Before switching rendering, add pak-loader fixtures (mixed skinned, masked,
double-sided, ProjectLit and multiple LODs), streaming/override churn,
in-flight snapshot reuse, and Main/depth/shadow screenshot parity. Record
draw counts and per-class high-water capacities.

No Vulkan build, pak integration or GPU repro was performed in this PR.
