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


## Stage 3: native indexed range publication (draft implementation)

`StaticGeometry` now records one `IndexedRange` per mesh at registration
time, from the same packed cluster-ordered index stream that is uploaded
to the GPU. On successful index/cluster upload, `indexedRangesResident`
allows these descriptors to be used; reset and upload failure leave them
unavailable. `WorldRenderer` publishes this native descriptor source
to `PersistentGeometryRegistry`, instead of recalculating an indexed range
from GfxFilter's per-cluster records. The existing persistent geometry key,
transient alias map, GPU-scene raster work ABI, and fallback policies are
unchanged.

**Not a complete GfxRenderer retirement:** `GfxRenderer::State` currently
inherits the `StaticGeometry` storage and its registration still supplies
revision-local IDs to `WorldSynchronizer`. This stage removes a redundant
descriptor *derivation*, not the shared physical geometry registration,
cluster-index buffer, optional task/mesh geometry, Hi-Z services or direct
fallback. The filtered rendering gate stays disabled. Subsequent migration
should make GPU-scene geometry the caller-owned registration authority and
replace transient IDs with residency-safe persistent references before
deleting the compatibility registration sidecar.

The indexed raster backend remains the Vulkan 1.3 baseline, independent
of optional `VK_EXT_mesh_shader` / task-shader support.

**Validation required before merging:** compile on Windows/Linux, run
`GpuSceneWorldTests`, and capture Vulkan mixed-scene frames through stream
promotion/eviction, multi-flight reuse, masked materials and fallback.
Tests added here exercise indexed range readiness, overflow, and
persistent identity; hardware parity has not been verified.

## Streaming publication failure handling (residency safety)

A streamed promotion/eviction invalidates the GPU-scene geometry revision.
The `WorldSynchronizer` refreshes each live mesh's resident mask and LOD/
submesh rows in-place, without rebuilding draw-item identity. If any DB
submesh, LOD, or mesh update fails (for example, due to a stale generational
handle), geometry publication now fails closed for that flight. The renderer
will not upload or admit GPU-scene draws from a partially refreshed snapshot.
The topology and geometry revision cursors are left unchanged on failure, so
the next frame retries the publication instead of considering the failed
revision complete. The direct renderer remains the fallback.

### World-native indexed physical ranges

The indexed raster publisher now consumes `World::LodChain::Level::mesh`
physical `Surface::firstIndex/indexCount/baseVertex` directly. These are
the same static vertex/index allocations the direct renderer uses, including
the same promotion/eviction retirement mechanism. The separate cluster-ordered
index copy is **not** used for indexed indirect drawing. A missing resident
LOD, invalid triangle range, an out-of-bounds physical allocation, or an
incompatible alias publishes a zero-index tombstone and fails admission to
the direct fallback. `WorldRenderer` refreshes the physical indexed ranges
on registration changes and after streamer promotions/evictions.

The optional task/mesh path retains cooked cluster/meshlet streams and a
separate upload; these meshlets already contain heap-global vertex IDs and
must **not** apply the raw indexed descriptor's baseVertex a second time.
Task/mesh dispatch still checks its packed-stream readiness, whereas
Vulkan 1.3 indexed raster can operate when that optional stream is
unavailable.

The implementation still uses the compatibility Gfx registration *number*
for correlating shared mesh LODs with the task/mesh tables, but the raster
**physical range** comes from World, not GfxFilter. Eliminating the
revision-local registration number and allocating native generational IDs
end-to-end remain future work.

**Not yet verified as full device parity:** Vulkan flight-retirement,
GPU-LOD demand feedback, promotion/eviction, mixed static/dynamic scenes,
temporal occlusion, and hardware image captures all need testing. In
particular, the GPU-driven LOD selector does not by itself guarantee the
GeometryStreamer's CPU demand/touch bookkeeping receives equivalent signals.

