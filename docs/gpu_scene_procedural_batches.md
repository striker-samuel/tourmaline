# GPU Scene: procedural batch producer contract (phase 1)

## Motivation

Placement currently maintains a separate path for GPU tile instance buffers,
visibility, prototype bins and indexed indirect draws. GPU Scene implements
persistent records and its own visibility/raster work pipeline. Procedural
instances should not become Scene nodes, and moving all 40-byte placement
records into 128-byte GPU Scene rows would lose their compact representation.

## Included here

`gpu_scene_procedural_batches.h` adds a **device-free metadata registry**
for GPU-resident batches, not a second per-instance registry. A producer
registers an opaque handle and publishes uniquely keyed batches. Each
batch names a format, source GPU address/byte range, prototype resolver,
count/capacity, world bounds and a monotonically increasing revision.

* Producer handles are never recycled; keys include a producer and local ID.
* Publish/retire are strictly versioned, preventing late generation from
  reviving stale tiles.
* Every successful publication gets a new incarnation.
* Snapshots are copies of metadata, not live mutable registry references.
* Empty tiles may publish without an instance buffer.
* Invalid layouts, insufficient buffer bytes, invalid bounds, unknown
  producers and stale revisions fail before mutating the registry.
* No GPU memory is owned by the registry. The publishing adapter must retain
  real buffer holders through the last consuming flight fence.

A `Placement40` record is **not** a `GpuScene128` record; the format is
explicit and consumers must select the correct shader fetch contract.

## Not included / remaining work

This phase does **not** modify PlacementWorldRuntime, WorldSynchronizer,
GPU Scene raster, or render ownership. No GPU draw or cross-format reinterpret
is enabled. The existing renderer remains authoritative.

Follow-up:
1. Build a Placement adapter that publishes a tile after generation, verifies
   dispatch completion and pins buffer/prototype lifetime per flight.
2. Implement procedural-batch candidate selection, generation checks,
   origin-relative culling, prototype/material residency and correct vertex
   fetch for the compact Placement format.
3. Extend exactly-once Main/depth ownership and fallback to procedural batches;
   keep existing placement rendering until GPU screenshot parity holds.
4. Verify all active views, shadow/motion passes, stream churn, frame fences,
   overflow, large-world precision and GPU timings before cutover.

Run `ProceduralBatchRegistryTests` (headless), then normal CMake/CI. No
Vulkan parity or performance claim is made by this PR.
