# GPU scene database — storage/publication phase

This is the first GPUIR-inspired **data infrastructure** step. GPUIR's
key separation is persistent render-scene tables vs transient visibility
and draw commands. This PR implements the former, not the latter.

## Tables and ownership

`tour::gpu_scene::Database<FrameCount>` holds independent tables:

- `Mesh`: LOD range, geometry residency mask, flags
- `Lod`: submesh range, per-mesh simplification error
- `Submesh`: geometry ID, material ID, pipeline bucket, pass mask
- `Instance`: DoubleFloat absolute position, float32 basis, local sphere,
  mesh/material/leaf references, flags
- `Leaf`: high/low world bound center, conservative half-extents, future
  contiguous membership range (NOT populated by this PR)

Every row is a standard-layout, 16-byte-aligned payload plus a 16-byte
`uint4 identity` trailer (`generation, alive, 0, 0`). CPU handles
are typed `Handle<T>{slot,generation}`. Deletion writes a tombstone and
bumps the generation; reuse does not let a stale handle refer to a new
row. The table also permanently retires a slot when incrementing the
generation would wrap.

Row strides: Mesh/Lod/Submesh **32 B**, Instance **128 B**,
Leaf **80 B**. See mirrored HLSL record declarations in
`engine/shaders/gpu_scene.hlsli`. No shader reads these records yet.

## Dirty publication and flight safety

Edits append a slot to each frame-flight's pending queue **once**.
Multiple writes before reuse coalesce into the latest payload. When
`Publish(flight)` is called, the relevant queue is sorted and adjacent
slots are combined into contiguous upload ranges. Publishing one flight
does NOT clear changes awaiting other flights. This is O(dirty log)
rather than O(all scene rows) each frame.

`gpu_scene_upload.cpp` is an optional RHI publisher: each table has an
independent storage-buffer mirror for every frame in flight. Buffers
grow geometrically; reallocation uploads the complete current table,
ordinary updates upload only coalesced dirty byte ranges. Old holders
retire through ResourceManager's existing resource lifetime machinery.
A failed allocation leaves queued changes available for retry.

**Caller contract**: call before recording consumers, on an acquired
flight slot whose previous GPU use has completed. Call the existing
RHI upload flush and declare the appropriate buffer hazards before
GPU reads (future integration). Do not reuse a flight while it is
still in use. Do not use an incomplete partial publication.

## Deferred integration

- This module does NOT yet mirror World nodes or entity spawn/destroy
  events. No render thread constructs a Database/Uploader yet.
- No individual instance culling, leaf membership, LOD selection,
  draw generation, or geometry residency publication is wired yet.
- No changes to GfxDraw, temporal seed/remainder, HZB, materials,
  or rasterization.
- Future consuming shaders should read `GpuScene*Row`, validate
  generation + live bit and resolve each high/low instance position
  against the per-view origin before any float32 culling/raster math.

## Tests

`GpuSceneDatabaseTests` (CPU/headless) exercises:
- new rows, slot reuse, stale handles and tombstones
- per-flight missed updates and coalescing multiple edits to one slot
- sorted contiguous upload ranges
- failed publication retry and forced complete snapshots
- independent tables, no-update frames and 100,000 instances with
  a single-row change

The Vulkan upload publisher is not device-tested in this PR; it needs
integration/validation on a running renderer before use by GPU shaders.
