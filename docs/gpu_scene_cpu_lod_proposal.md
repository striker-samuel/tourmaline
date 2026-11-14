# RFC / draft: CPU shallow LOD selection with GPU-scene consumption

Status: **proposal + device-free reference implementation**, not a production
renderer cutover. This branch deliberately leaves GPU LOD work selection,
indirect draws, and legacy fallback unchanged.

## Rationale

The CPU can evaluate LOD using a **shallow** scene representation (bounds,
geometric error, residency, and stable identity), without decoding or touching
vertex/index data. GPU scene remains the authority for physical geometry,
generation-checked residency, per-flight uploads, visibility/Hi-Z and draw
generation. This decouples an independently testable LOD policy from raster
ownership and creates a natural place to request finer nonresident LODs.

Current master already has GPU projected-error, resident-LOD selection
(#286); this is a proposed alternative or optional hybrid, not a missing
feature. Headless metadata publication already exists in
`gpu_scene_world.h` and `docs/gpu_scene_lod.md`.

## Prototype shipped in this draft

`engine/include/Engine/Renderer/gpu_scene_cpu_lod.h` exposes a device-free
`cpu_lod::Select` policy for up to 16 fine-to-coarse shallow LOD rows.
It returns:
- `desired`: coarsest LOD meeting projected geometric error, even if absent;
  a streamer may use it as demand input.
- `resident`: matching or finer eligible resident LOD, otherwise nearest
  coarser eligible resident LOD. Invalid means **fallback**, not empty draw.

Invalid scale, error, incomplete bounds, nonfinite metadata, and no eligible
resident LOD fail closed. The caller supplies a conservative
pixels-per-world-unit scale using camera/depth metadata. This oracle does
not itself compute a camera projection, apply hysteresis, or publish tables.

## Proposed production contract

1. Maintain shallow instances with stable source key, generational mesh
   handle, conservative bounds, transform revision, optional previous choice.
   Share the LOD metadata across instances of the same mesh; do not copy
   entire LOD chains per instance.
2. Run CPU selection on relevant visible/near-visible leaf or spatial-cell
   candidates. Use the actual camera projection and conservative near-depth
   treatment. Add hysteresis and invalidation on camera, transform, topology,
   residency and geometry-registration revision changes. Avoid a full scan
   of all world instances every frame.
3. Publish the chosen LOD **per instance / per view**, not by mutating a
   shared Mesh row (two instances of one mesh can need different LODs).
   Prefer a separate generational selection table or per-view work input.
   Include mesh slot+generation and geometry publication revision, with an
   explicit invalid sentinel. Preserve the existing shared Mesh/Lod/Submesh
   metadata and the per-flight snapshot barrier.
4. On the GPU, validate instance and mesh generations, selected row index,
   residency, bounds and physical surface identity before dereferencing.
   On stale/missing choices, use the existing conservative resident selector
   or legacy spill. Never turn an invalid selection into an invisible object.
5. Keep GPU frustum/Hi-Z culling, temporal history and indirect command
   generation. This proposal only changes **LOD policy placement**, not
   GPU-driven draw ownership.

## Important tradeoffs

- A single mesh-level `selectedLod` is incorrect for shared meshes.
- CPU selection may be expensive for millions of instances and multiple
  cameras/shadow views; spatial hierarchy, work budgets, batched updates
  and a GPU fallback need benchmarking.
- CPU visibility cannot substitute for GPU occlusion; use conservative
  shallow candidate generation so newly visible objects never disappear.
- Host-to-device synchronization and stale selection after residency
  eviction are correctness risks. Per-flight publication and generational
  validation are mandatory.
- Streaming demand must distinguish desired quality from resident quality.
- Compare against the already-shipped GPU selector on real scene/camera
  captures before any default-path switch.

## Acceptance criteria for a follow-up cutover

- [ ] Device-free test matrix: projected-error thresholds, high/low distance,
      invalid metadata, no residency, fallback, shared instances with
      divergent choices, generation reuse, camera projection, hysteresis.
- [ ] Per-view selection upload and generational validation on Vulkan.
- [ ] Multi-flight/streaming promotion+eviction correctness with no missing
      draws and no stale reads.
- [ ] Shadows/G-buffer/motion/temporal/Hi-Z parity against GPU selector.
- [ ] Large-world precision and multiple cameras validated.
- [ ] CPU selection/upload cost vs GPU saved work measured on dense scenes.
- [ ] Optional feature flag / A-B capture comparison, legacy fallback kept.

This draft is intentionally **not** claiming GPU parity or performance gains.
