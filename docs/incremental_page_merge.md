# Incremental page merge: stable resource addressing

Status: **design note — direction agreed, not implemented.**

Context: `World::ResolveAndFixup_pages` is the multi-page "legacy fold" that
the camera page streamer (`ScenePageStreamer`) drives. Today every page
add/remove triggers a full wipe + re-fold of every resident page, so a
streamed-in page pays O(total nodes) copy + O(meshes log meshes) draw-list
sort + O(static log static) BVH rebuild. That is why the island's frame time
climbed to ~2.5 s as pages streamed in, even with only a few resident.

## Why it cannot be incremental yet

A node's resource references are **index-addressed**, not identity-addressed:

- `scene.tables.meshes[node].mesh` stores a *world mesh index* assigned in
  fold order, resolved through `World::meshLocations[index] = { WorldPage*,
  pool handle }`. Materials/cameras/decals/lights are the same shape.
- Every fold wipes and re-emits those indices, so a page add/remove renumbers
  every later resource index — which forces the whole fold (and its sort +
  BVH tails) to re-run.

## Decision: stable URI-hash keying (now)

Bind a node to its resource by a **stable key**, not a slot index:

- The resource already has a stable identity — its **VFS URI** (registered via
  `RegisterMeshUri`/`RegisterMaterialUri`/…). Key it by `PakHashPath(uri)`.
- `meshLocations`/`materialLocations`/`lightLocations`/`cameraLocations`/
  `decalLocations` become **hash-keyed maps** (`hash → { WorldPage*, handle }`)
  instead of dense vectors.
- A node's `MeshRow`/`MaterialRow`/… carries the hash.

Then the fold decomposes into page-scoped deltas:

- **Add a page** = insert its `(key → handle)` entries (or find-existing for a
  shared resource) + append its nodes/links/flags/LOD rows. Nothing resident
  renumbers.
- **Remove a page** = drop its keys + retire its node slots. Remaining
  bindings do not move.
- The O(n log n) tails (draw-list `stable_sort`, static BVH) are rebuilt once
  per *settled batch*, not inline per page.

This is the foundation for Epoch 4/5 of `docs/world_tile_streaming_epochs.md`
(stable asset-owned records, remove `clusterMergedNodes` + the full
`ResolveAndFixup_pages` on the streaming path).

## Future work: content-addressed asset IDs

`PakHashPath(uri)` is stable per-cook but only unique per-pak. The shared
blob pool already dedups by URI within a pak, so URI-hash keying gives
single-pak shared-resource dedup for free. But it does **not** survive
cross-pak references (a prefab pak referencing a world pak's mesh): two paks
can both own the same URI with different content.

When cross-pak prefab references land, the key must become the
**content-addressed `#ids` id** (the `0x3xxx` mesh id / `0x4xxx` sound id in
the per-pak `#ids` table), not the path hash. The runtime already has the
`#ids` identity lane (`ReadWorldManifest`'s hash + the `#ids` blob); the
resource-location maps would key on that id instead of the URI hash, and the
resolver joins across the mounted pak providers. Deliberately deferred: the
island is a single pak, so URI-hash suffices now.
