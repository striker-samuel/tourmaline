# GPU Scene v3: shared-mesh LOD/residency publication

The GPU-scene database (PR #18) and stable World instance registry
(PR #22) are prerequisites. This phase fills the **mesh**, **LOD** and
**submesh** rows from authoritative World/GeometryStreamer state while
leaving existing CPU LOD selection and GPU meshlet rendering unchanged.

## Identity and relations

- A shared base mesh keeps one permanent generational Mesh row while
  referenced by live instances. Camera movement does not touch this row.
- Mesh.firstLod points at an **actually contiguous** block of Lod rows;
  Mesh.lodCount is the complete cooked chain, including deferred levels.
  The table's InsertContiguous allocator reuses contiguous free runs
  where possible, without relying on independent free-list Insert calls.
- Each Lod has one **provisional** submesh record at Lod.firstSubmesh.
  This represents an entire filterable gfx mesh in v3; it is *not yet*
  the finished surface/material/pass breakdown from GPUIR.
- Each resident/filterable Submesh.geometryId is a snapshot of
  GfxRenderer::GfxMeshForLod(worldMesh, level), whose indices currently
  change whenever GfxFilter static registrations are reset. The
  WorldRenderer increments a geometry registration revision on each
  rebuild and refreshes these references. Persistent mesh identity
  never depends on those transient ids.
- Material ID, pipeline group and passMask remain invalid/zero; NO
  shader may issue a draw from these provisional rows.
- Chain reshaping retires old LOD/submesh records in flight-safe
  generational tombstones and replaces them with a new contiguous span.
  A simple promote/evict only changes existing rows.

## Residency and bounds

A mesh's residentMask describes physical GPU geometry payload
residency, NOT eligibility for the meshlet culling path.
Per-LOD flags:

| Flag | Meaning |
|---|---|
| LodFlagResident | Geometry payload present |
| LodFlagBoundsKnown | Bounds available even when payload is deferred |
| LodFlagFilterable | Resident AND current gfx meshlet registration valid |

A deferred sibling's `DeferredMeshLod::src.aabbMin/aabbMax` survives
promotion and eviction; the World LOD-chain cache now retains those
bounds in both resident and deferred states. The instance's local
bounding sphere is derived from the AABB **union across all levels**,
not only LOD0. When any bounds are unavailable, the Mesh flags do
not include `MeshFlagCompleteBounds`. Future GPU instance culling
MUST bypass/reject a culling decision for that mesh until bounds
become complete. The current renderer's CPU BVH is unchanged.

## Publication and synchronization

- WorldSynchronizer does the full mesh metadata pass only when World
  topology changes OR the gfx mesh registration revision changes.
- Per-LOD promotion/eviction triggers the existing World::RebuildLodChains
  and gfx re-registration, then bumps the revision.
- When comparing old/new rows, unchanged Mesh/Lod/Submesh data is NOT
  enqueued. Only updated residency masks, flags, geometry IDs and
  changed per-LOD records reach the existing per-flight Uploader.
- In-flight safety relies on the existing ResourceManager retirement
  and each flight's complete scene snapshot. A future consumer must
  enforce resource hazards and generation validation for every
  cross-table dereference. This PR adds no GPU read consumer.

## Limitations / follow-ups

- Some deferred or nonresident assets may lack complete bounds metadata.
  They are explicitly NOT marked safe for GPU instance culling.
- Gfx mesh registration still rebuilds the entire existing filter
  table on streaming changes. This PR does NOT replace that allocator.
- Skinned and direct-only geometry do not produce a filterable
  submesh ID, even when physically resident.
- GPUIR per-surface submesh/material/pipeline batching, GPU coarse
  leaf selection, persistent temporal LOD state and GPU streaming
  feedback are future phases.
- World and camera transforms still originate as float32, so
  DoubleFloat transport here does not yet enable true LWC.

## Validation

Run `GpuSceneLodTests`, `GpuSceneWorldTests`,
`GpuSceneDatabaseTests` and `HeaderSmoke` on both toolchains.
The new headless CTest checks contiguous slot reuse with fragmentation,
GPU referential identity, LOD promotion/eviction, registration remaps,
missing-bounds safety, per-flight dirtiness and chain reshaping.
