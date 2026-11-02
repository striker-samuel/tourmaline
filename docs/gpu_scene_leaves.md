# GPU Scene v4 — spatial leaf database

Follows PR #31 (persistent mesh/LOD residency). This stacked PR
implements the GPUIR idea that a **coarse spatial leaf references a
range of persistent instance IDs**, independent of material-sorted
per-view draw records. No CPU or GPU visibility traversal is enabled.

## Spatial ownership

- Leaf width: **64 metres** in XZ, matching Tourmaline's documented
  WorldPartition subcell resolution. Each instance is *owned by* the
  leaf containing its absolute translation (floor semantics for
  negative coordinates). An object's full bounding sphere is included
  in its leaf AABB even when it crosses tile boundaries.
- Each Leaf has a persistent generational row in `Database.leaves`.
- `Leaf.range` contains an actually contiguous span in
  `Database.leafMembers`: `x = first row`, `y = count`, and `z bit 0`
  indicates bounds complete enough for **future** group culling.
- Each LeafMember records an instance `(slot, generation)`; a future
  consumer must validate both the LeafMember row and the referenced
  Instance row generations. No frame-local GfxDraw index is used.
- When the membership length changes, the registry allocates a fresh
  contiguous range and retires old rows via generation-stamped
  tombstones. A stable leaf handle survives membership changes.
- Membership order is deterministic, sorted by instance slot/generation.
  Leaves are ordered by signed `(cellX, cellZ)`.

## Incremental updates

The WorldSynchronizer consumes the SceneRegistry's changed/retired
instance journal after reconcile or transform recalculation:

1. Retire old instance membership using exact generational handles.
2. Upsert changed/created instance identities into the appropriate cell.
3. Recompute only touched old/new leaf bounds and member ranges.
4. A mesh/LOD/residency revision invalidates *leaf safety*, without
   changing membership or its ranges.
5. Publish Leaf + LeafMember table dirty rows using the existing
   per-frame-in-flight Uploader.

Camera movement alone produces zero scene membership changes and
zero leaf-table uploads. Instances moving within a cell do not rewrite
their membership rows. Cell crossing updates two leaves, not every
instance in the world.

## Conservative-bounds policy

A leaf is `LeafFlagBoundsComplete` only when ALL its instances
reference a live, generation-matching Mesh with
`MeshFlagCompleteBounds` (full cooked LOD bounds), and their
transformed local spheres are finite. To accommodate arbitrary affine
shear, radius scale is bounded with the Frobenius norm of the 3×3
basis; the result is conservative, though looser than a spectral-norm
bound. Half-extents are rounded outward, and the center is transported
via DoubleFloat. No GPU shader currently tests or trusts these bounds.
Future GPU rejection must still use consistent origin-relative
arithmetic and include its own roundoff policy.

Invalid/non-finite/out-of-range cell positions go to a non-cullable
sentinel leaf. Unknown mesh bounds never lead to dropped instance IDs.

## Deliberately deferred

- No WorldPartition streaming policy change or global CPU BVH removal.
- No CPU coarse frustum traversal or `PassedInstanceRanges` yet.
  `WorldSynchronizer::LeafTickets(db)` exposes the published
  membership range and safety state as a basis for that next step.
- No instance visibility, GPU LOD transitions, meshlet expansion,
  draw batching or rasterization changes.
- Submesh material/pipeline IDs from PR #31 are still provisional.
- CPU Scene.globalTransform is float32; DoubleFloat transport cannot
  recover precision it lost. Upgrade CPU transforms before enabling
  genuine large-world GPU culling.
- GPU-scene table read barriers / shader descriptor publication are
  required when a GPU consumer is eventually added.

## Tests

The headless `GpuSceneLeafTests` target validates: 64m negative
floor semantics, stable leaf identity, contiguous member rows,
slot-reuse generation checks, motion within and across cells,
no-op dirty publication, incomplete-bounds bypass, large-world
positions, sheared transforms, and mesh deletion.
