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

## Stage 2: authoritative indexed raster consumer (follow-up PR)

The live `WorldRenderer` now synchronizes `PersistentGeometryRegistry`
from the same GPU-scene database used by `WorldSynchronizer`, after
database publication and on the acquired frame flight. It publishes stable
indexed geometry descriptors and a *per-flight* transient registration →
persistent descriptor mapping.

The indexed command-build compute shader consumes the transient ID in
`SceneSubmeshWork.identity.z` only as an index into this validated,
flight-local mapping. The resulting descriptor lookup is a persistent
slot; any unregistered or invalid entry writes an empty indirect command.
This avoids exposing the current revision-local GfxFilter registration
numbers as descriptor addresses.

The admission loop checks that **each resident LOD** has a mapped
descriptor with exactly the same indexed range as the current registered
physical payload before giving GPU Scene ownership of an object. A
missing or mismatched mapping leaves that object legacy-owned. Persistent
descriptor publication failure disables GPU scene rendering for the frame.
The existing per-instance generation check remains authoritative in the
raster vertex fetch, and the descriptor mapping is captured separately
on each acquired flight.

**Not yet done:** the physical index heap is still the GfxFilter cluster
heap, and the temporary work identity still contains revision-local
registration IDs. GeometryStreamer must expose the underlying index/vertex
draw range directly, with a stable residency identity, before this
compatibility mapping can be deleted.

The 65,535 geometry-slot cap remains fail-closed. No unsafe slot recycling
is added here: generational GPU work references and a recorded per-flight
retirement fence are required before implementing reclamation.

Device validation remains required. In particular, test multiple frames
in flight with LOD promotion/eviction, re-registration, missed residency,
masked foliage and no GPU scene ownership on a failed indexed publication.
A headless test cannot replace these Vulkan captures.
