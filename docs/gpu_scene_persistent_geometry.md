# GPU Scene persistent indexed geometry — stage 1

## Why this exists

The existing four-bin GPU-scene raster path still expands a transient
`gfxMesh` registration into `SceneSubmeshWork.identity.z`. Its indexed
geometry descriptors are currently in the same revision-local order as
`GfxFilter`'s mesh registrations. Resetting that registration list can
renumber all work, so those IDs cannot serve as long-lived raster identity.

## New CPU authority

`PersistentGeometryRegistry` (in
`engine/include/Engine/Renderer/gpu_scene_raster_geometry.h`) keys each
indexed geometry slot by:

- Persistent GPU Scene mesh table slot.
- **Mesh generation** (rejects slot reuse after retirement).
- Selected LOD index.

`Synchronize(RenderDatabase, IndexedGeometryTables)` rebuilds the set of
*currently drawable* ranges, using the registered filter index ranges only
as a compatibility source for locating the physical index payload.

A missing range, nonresident LOD, or retired mesh is represented by an
indexed descriptor with `indexCount=0`. The old persistent slot remains
allocated, and a different mesh generation is assigned a different slot.
Malformed LOD chain metadata aborts the rebuild without publishing a
partially updated snapshot. Registry revision changes only when descriptor
rows change.

`FromTransient` provides an optional short-lived conversion from the
current GfxFilter registration ID to a stable registry slot. This mapping
**must never be cached** across geometry-registration revisions. Multi-LOD
aliases are supported only when they resolve to the same indexed range;
conflicting aliases fail publication.

## Per-flight GPU descriptors

`PersistentIndexedGeometryPublisher` publishes a 16-byte
`IndexedGeometry` array keyed by persistent slot to the acquired flight's
own GPU buffer. Reallocation uses ResourceManager Holder retirement.
Unmodified registry revisions avoid repeated uploads. Invalid slots emit
zero-index indirect commands in the existing draw builder.

Both the existing revision-local `IndexedGeometryPublisher` and its
legacy call sites are left untouched. This first stage does **not**
change the live raster command producer or alter existing GfxFilter
fallback. It is safe to merge without silently switching GPU geometry.

## Follow-up for authoritative cutover

1. Wire registry synchronization from WorldRenderer after
   WorldSynchronizer's geometry/LOD publication, using the same acquired
   flight and its scene DB snapshot.
2. Publish the persistent descriptor buffer, then switch GPU work records
   from revision-local `geometryId` to the stable slot (or a checked
   per-flight mapping). Check work generation and descriptor validity
   before issuing indirect draws.
3. Remove dependence on `GfxFilter` registered mesh IDs entirely by
   sourcing GPU Scene's index/vertex descriptors from GeometryStreamer
   residency metadata. The current resolver still reads its compatible
   cluster-ordered index heap.
4. Add GPU/device parity with streaming promote/evict, mixed LODs,
   re-registration, multi-flight retirement and missing metadata.
5. Introduce flight-fenced slot recycling with a generational GPU work ABI
   if descriptor slot pressure exceeds the 65,535-slot safety cap.

## Tests

`GpuSceneWorldTests` covers registration renumbering, no-change
revisions, eviction/re-residency tombstones, mesh-generation reuse and
transactional failure on invalid LOD spans. Hardware GPU publication and
renderer end-to-end cutover are not claimed by these headless tests.
