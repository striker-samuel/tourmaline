# GPU Scene v2 — World synchronization

Implements the first real integration of the GPUIR-style persistent database
(PR #18) with Tourmaline's World and Scene, without changing draws.

## Stable source identity

- World::RegisterCluster assigns a monotonically increasing registration
  ID. Unload/unregister removes the ID; even if an allocator reuses a
  Cluster pointer, a new registration gets a distinct identity.
- GPU instance key = (cluster registration, cluster-local NodeId,
  cluster-local node generation). It is independent of the merged Scene's
  NodeId and the material-sorted World::drawItems order.
- GPU mesh key = (owning cluster registration, mesh pool handle index,
  mesh pool handle generation). Multiple instances share one mesh row.
- World::clusterMergedNodes is reversed only when a world resolve changes
  topology. The renderer's WorldSynchronizer retains this node-to-key map
  across ordinary frames.

## Update and lifetime flow

1. World::ResolveAndFixup_clusters increments gpuSceneTopologyRevision
   after rebuilding drawItems. The synchronizer reconciles the complete
   renderable set only on a new topology revision: it adds instances,
   refreshes changed bindings and writes tombstones for missing sources.
2. recalculateGlobalTransforms / recalculateGlobalTransformsDynamic append
   each successfully computed global-transform node to
   Scene::gpuSceneGlobalChanges, **before** clearing the original hierarchy
   dirty queues.
3. On ordinary frames, WorldSynchronizer sorts/dedupes this journal and
   updates only its affected persistent instance records. Identical data
   is not published again.
4. WorldRenderer calls the synchronizer and the database Uploader on the
   acquired frame flight, after geometry streaming updates and before
   draw recording. The per-flight database publisher handles range
   coalescing, allocation growth and retired GPU buffer references.
5. A new World pointer clears old bindings by publishing tombstones
   before reconciling the new source (source IDs are World-local).

## Important limits, not yet ready for GPU culling

- Scene authoritative transforms are currently float32. The bridge
  serializes them into DoubleFloat but **cannot restore lost precision**;
  CPU double-transform migration is still outstanding.
- New GpuScene Mesh records deliberately have lodCount=0 and
  residentMask=0. No geometry or LOD sibling table is published yet.
  GPU consumers MUST treat these as unresolved.
- Instance localSphere currently represents only the base mesh AABB
  sphere; GPU visibility must first publish full-chain conservative
  bounds.
- Instance.refs.y is a refreshed World material-table index, not a
  generational GPU material ID. The material table publisher is pending.
- Leaf memberships are not populated. Visibility changes that do not
  rebuild World drawItems or recompute transforms are not yet mirrored.
- This uses ONE render-thread journal consumer. Direct unmarked writes to
  Scene.globalTransform bypass the journal and are unsupported.
- GPU scene upload buffers are currently **unreferenced by all shaders**.
  There is no switch to GPU culling, batching, indirect draw generation,
  HZB or seed/remainder execution in this PR.

## Validation

Run `GpuSceneWorldTests`, `GpuSceneDatabaseTests`, `HeaderSmoke`
and full CI. The new headless test checks sorted-list reordering,
shared mesh rows, transform-only edits, no-op updates, unload/reload
identity, mesh retirement, and post-recalculation scene journals.

The GPU publication path additionally requires an acquired/fenced
frame slot, transfer completion and barriers before any future GPU
consumer reads it. No Vulkan device tests are added here.
