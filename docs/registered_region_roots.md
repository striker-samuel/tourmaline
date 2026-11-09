# Registered region roots: transition and target architecture

This branch introduces a **region registry over World::scene**. A region is
a designated generational node root, not another Scene. The nearest registered
ancestor determines its node-lifetime ownership boundary. Resource lifetimes
are independent from that boundary.

## Implemented in this branch

- `RegionRegistry` stores monotonic region IDs, generation-checked roots,
  a root-to-index lookup, and nearest-ancestor membership.
- `World::regions` and its registration, removal, and query methods provide
  an explicit world-level API.
- The transitional `ResolveAndFixup_clusters` path registers each imported
  cluster subtree as a region. Registrations are discarded when that path
  rebuilds the entire Scene, preventing stale slot identity from carrying
  across a reset.
- Editor reparenting is allowed inside an ownership region and rejected
  across registered boundaries; moving ownership across regions requires an
  explicit future transfer operation.

## Target invariants

1. One authoritative `World::scene`; no independent cluster-local scene
   graphs or mirror node identities.
2. A node's parent is immutable after creation at runtime. The authoring
   editor may change hierarchy within an ownership boundary.
3. A region owns node *lifetime*, not referenced mesh/material/texture assets.
4. Additive scene and prefab instances register individual roots. Prefab
   transforms change the root's transform, not hierarchy topology.
5. Region removal retires its subtree and invalidates handles, while
   externally held assets survive until their last reference is released.
6. Runtime GPU instance identity is a world-qualified generational node
   handle; material/mesh asset identities remain independently stable.

## Remaining migration (not implemented by this branch)

The bridge above is NOT the final single-scene architecture. In particular:

- Replace `Cluster::scene` import/resolve and `clusterMergedNodes` with direct,
  incremental append/retire of stable world slots. Do not invoke a full
  world-scene rebuild on scene load/unload.
- Replace `Cluster` resource pools and `retainedClusters` with asset-owner
  records and independent resource retention, including pak-image pins and
  deferred GPU allocation retirement.
- Change `ProjectScenes` load/additive/instantiate/remove to allocate
  world subtrees directly; preserve scene generational handles, prefab root
  transforms, asset prefetch/hold semantics, and unload.
- Remove entity mirror nodes. Audio, mesh entities, animators, and physics
  bindings should refer directly to generation-checked world node identities.
- Publish incremental creation, retirement, transform and mesh-binding
  changes to the renderer's GPU-side draw state instead of relying on
  cluster source identity and full topology reconciliation.
- Teach editor reimport/prefab overlay/save/undo to distinguish document
  ownership from region ownership, and provide an explicit cross-region
  transfer operation.
- Restrict runtime-facing hierarchy mutation APIs to creation with a
  permanent parent, deletion, and local transform updates. Authoring may
  retain reparenting.

The integrator owns build/correctness verification, including region handle
lifetime, scene unload/reload, shared resources, prefab instances, and editor
undo/redo. No tests were run in preparation of this branch.
