# smvk table core extraction

The generational slot table formerly defined in `gpu_scene.h` now lives at
`Engine/RHI/table.h` in `smvk::table`. GPU Scene retains public aliases for
`Row<T>`, `Handle<T>` and `Table<T, FrameCount>`, preserving ABI, shader
layout, and existing call sites.

## Ownership boundary

- `smvk::table::Table` owns canonical CPU rows, generation/liveness,
  free slots, per-flight dirty journals and merged contiguous write ranges.
- The GPU-scene uploader continues to own per-flight GPU buffers, page-quantized
  capacity growth and physical RHI publication.
- `smvk::ResourceManager` continues to own staging, barriers, VMA allocations,
  and deferred physical retirement.

This is a **mechanical extraction**, not the old material resolve/diff proposal.
It does not introduce a second material shadow, replace WorldPage material
pools, or migrate material handles yet.

## Next layer (separate PR)

Make CPU row identity independent of GPU serialization through an explicit
layout/stream policy (AoS material payloads vs SoA streams). Preserve logical
dirty rows and 64-KiB physical growth pages; do not upload a whole page for
one dirty row. Then migrate canonical material ownership into smvk with
dependency invalidation for bindless remaps and a shader-index ABI transition.

## Verification

Existing `GpuSceneDatabaseTests` still cover generational reuse, tombstones,
per-flight convergence, dirty range coalescing and writer failures. Added type
identity assertions prove existing GPU-scene public types resolve to the new
smvk core. Build/test execution is required before merge.
