# World tile streaming: cluster retirement epochs

Status: **Migration in progress — NOT a completed cluster replacement.**
Base: the existing WorldPartition, SMP8 paged pak container, RegionRegistry,
ContentRegistry/AssetStore, and persistent GPU-scene work (#224/#229).

## One active runtime World, multiple authored Maps

- `tour::World` / `m_world` is the single active runtime simulation object.
- `tour::Map` is the durable world-wide spatial distribution, identified by
  stable `WorldId`. Multiple Maps are project assets; **one Map is active**.
- `WorldTileKey` is `(WorldId, cell)`; world A's `(0,0)` is unrelated to
  world B's `(0,0)` even when stored in one archive.
- Map switching must first invalidate outstanding async request tickets,
  retire old regions/assets safely, and only then bind the new Map manifest.
- No concurrent runtime worlds, multi-Map residency, or additional world
  instance identifier is required.
- The CPU SceneTileProvider now enforces one Map per manifest; host-level
  switch, renderer and GPU fence cutover still require implementation.

## Map versus Scene (new decision)

- `tour::Map` is the world-wide authored hierarchy of anchors and placed
  local Scene asset references. `tour::scene::Scene` is a reusable local
  hierarchy for interiors, prefabs and closed spaces, not an open world.
- `tour::World` is the runtime state; Map and Scene authoring documents are
  not alternate mutable runtime worlds. See `docs/map_scene_world_architecture.md`.
- Initial code now provides `WorldMap.h` Map hierarchy compilation into
  deterministic logical tiles, and `map_tile_codec.h` for a bounded LE tile
  placement page. This is not yet connected to standalone/Project Build,
  pak writing, or world instance activation.

## Architectural contract

1. `WorldPartition` decides **residency**, considering gameplay/camera interests,
   velocity lookahead, budgets and hysteresis. Per-view frustum/occlusion only
   decides **visibility** of resident content. Never make residency depend
   exclusively on the current camera frustum.
2. A **world tile** is a logical content manifest cell, not a pak. It may be
   stored as one independently readable SMP8 page inside a regional pak.
   Shared meshes/materials/textures keep independent stable asset IDs.
3. Cooked tiles contain immutable render-instance data, authored properties,
   dependency identifiers, conservative bounds and static spatial acceleration.
   Mutable gameplay entities live under persistent runtime ownership.
4. The only mutable runtime graph is `World::scene`. Streaming activates
   and retires generational world nodes incrementally; it never causes a
   full-world `ResolveAndFixup_clusters` rebuild.
5. CPU-ready does not mean published. An instance becomes visible only after
   its assets, GPU uploads, descriptors and scene database entry are ready.
   Retirement waits for outstanding GPU submissions.
6. A cooked BVH stores **tile-local** conservative bounds including all
   selectable LODs. Its leaves refer to cooked local instance keys, NOT
   transient world node slots or `Cluster*` addresses. A coarse world index
   filters tile/instance roots; dynamic objects use a separate mutable index.
7. Streaming cell ownership and scene document ownership are distinct.
   Cross-cell large objects have one logical owner; persistent gameplay
   entities are independent of streamed visual tile lifetime.
8. Existing small non-tiled scenes/prefabs remain supported as the same
   cooked scene format instantiated without a spatial tile owner.

## Epochs and integration gates

### Epoch 0 — ownership, formats and fixtures
- [ ] Formalize `CookedScene`, `CookedTile`, `SceneInstanceId`,
  `AssetHandle`, `NodeHandle` and `CellKey` invariants.
- [ ] Pin test scenes: empty tile, many repeated mesh instances, shared
  mesh across cells, skinned entity, decal/audio, nested prefab and a
  tall cross-cell mesh.
- [ ] Measure current startup/resolve times, frame CPU stages, upload bytes,
  memory, shader/renderer fallback and pak page reads.
- [ ] Preserve compatibility until new path passes integration gates.

### Epoch 1 — scene tile interest/residency provider
- [x] Introduce CPU-only `SceneTileProvider` using
  `WorldPartition::StreamLayer::Scene`.
- [x] Manifest addresses logical tile IDs, independent of physical pak layout.
- [x] Demand collection, pending dedup, ticket-checked acknowledgement,
  cancellation and retired-cell notification with headless tests.
- [ ] Integrate into application render-thread WorldPartition provider wiring.
- [ ] Add priority queues, dependency resolution, velocity prefetch,
  hysteresis, bandwidth/IO/upload/memory budgets and cancel generations.
- [ ] Ensure the host's completion callback cannot revive a retired tile.

### Epoch 2 — cooker-produced tile and spatial records
- [x] Add a headless authored Map placement hierarchy, deterministic
  root-cell ownership projection, and a bounded, versioned LE placement
  page codec with CPU tests (preliminary format; not yet shipped by cooker).
- [ ] Partition finalized authored scene records into root-cell ownership,
  with explicit handling for objects spanning cells.
- [ ] Build deterministic static instance BVHs **offline after authored
  transforms, static classification, LOD union and mesh bounds are final**.
- [ ] Build coarse world-cell index; serialize local-instance references.
- [ ] Version the scene/tile format and check bounds, finite transforms,
  array extents, byte order, alignment, CRC and invalid leaf references.
- [ ] Run the same cook path from standalone cook, editor Cook and
  Project Build. No runtime BVH partitioning for cooked static scenes.
- [ ] Roundtrip and deterministic-recook tests with negative cells,
  static/animated exclusion, transforms and every LOD.

### Epoch 3 — logical paging, storage, and dependencies
- [x] Reserve a dedicated internal SMP8 `MapTile` kind and add
  `AddMapTiles` / `ReadMapTile` adapters for independent placement pages;
  exercise multiple logical cells in one archive using the real PakWriter
  and PakReader in a headless test. Does not yet integrate with Project Build.
- [ ] Add tile manifest entries to the existing paged SMP8 pak TOC,
  resolved through ContentRegistry and PakReader positional reads.
- [ ] Keep packaging policy separate: one pak, regional paks, or tile paks
  are all valid implementations of identical logical tile IDs.
- [ ] Add asynchronous IO/decode, failure and retry semantics, checksum
  handling, cancellation, deduplicated in-flight reads and telemetry.
- [ ] Verify multi-tile shared asset IDs and cross-pak dependency resolution.

### Epoch 4 — asset ownership independence
- [ ] Move mesh/material/texture/light/camera/decal asset records and
  skeleton/clip/bake references out of `Cluster` pools into stable
  asset-owned records or equivalent independently referenceable stores.
- [ ] Make load/unload leases independent of node-region ownership.
- [ ] Retire `Cluster::assetRefs`, parked clusters and `retainedClusters`
  after validating external references and delayed GPU destruction.
- [ ] Verify texture/pak image pins remain alive while borrowed data is
  accessed, even when a scene tile unloads.

### Epoch 5 — incremental authoritative World
- [ ] Instantiate tile/scene nodes directly into `World::scene` with
  world-qualified generational handles and explicit instance ownership.
- [ ] Implement transactional `ActivateTile` and `DeactivateTile`:
  check budgets and dependencies before publishing; rollback on failure.
- [ ] Avoid node-slot remapping on unrelated tile load/unload, and retire
  nested ownership deterministically.
- [ ] Remove `clusterMergedNodes`, source scene mirrors and
  `ResolveAndFixup_clusters` on the new loading path.
- [ ] Support standalone prefab instances and authoring undo/reimport
  with stable document IDs distinct from runtime instance IDs.
- [ ] Replace `EnsureDynamicCluster` with persistent world-owned runtime
  entity spawning and explicit promotion rules.

### Epoch 6 — GPU scene publication and spatial traversal
- [ ] Publish per-instance create/change/retire deltas with generation checks.
- [ ] Traverse cooked tile-local BVHs and a coarse tile index to produce
  visible-instance streams directly, without O(total scene nodes)
  visibility-mask initialization or full `drawItems` scan per view.
- [ ] Integrate `#224` instanced indirect planning and fallback for
  missing/skinned/lightmap raster semantics.
- [ ] Support shadow views, multiple cameras, transparent sorting,
  animation, dynamic overlays, HLOD and LOD residency changes.
- [ ] Guard GPU buffer/descriptor retirement with per-flight fences.

### Epoch 7 — integrator acceptance, cutover and removal
- [ ] Run all targeted CTests below, whole-project build and real Vulkan
  smoke tests; report failures rather than treating unrun tests as passes.
- [ ] Profile the existing 10,001 × 104 repeated-mesh case and publish
  before/after per-stage CPU ms, GPU ms, drawn/culled candidates, memory
  and page-read stats.
- [ ] Test streaming during camera spin, rapid travel, teleport, reload,
  hot reimport, canceled loads, corrupt pages and OOM.
- [ ] Once the new path is validated, delete the old Cluster loading,
  per-cluster fixups, parked-cluster and dynamic-cluster paths and tests
  that explicitly bind to those obsolete behaviors.
- [ ] Update architecture documentation and compatibility notes.

## Integrator test prescription

Configure with the repository's normal supported CMake preset. Build the
engine, app, tools, editor and test targets. Run:

```sh
ctest --test-dir <build-dir> --output-on-failure -R "scene_tile_provider_tests|world_map_tests|world_partition_tests|world_entity_tests|scene_table_tests|content_registry_tests|gpu_scene|streaming_tests|asset_tests"
ctest --test-dir <build-dir> --output-on-failure
```

Target names/patterns beyond `scene_tile_provider_tests` should be
confirmed against `ctest -N` for the selected preset.

Manual end-to-end acceptance:
1. Load several spatial tiles sharing the same mesh and texture; unload
   one and verify remaining references and renderer allocations survive.
2. Spawn an animated persistent entity, unload/reload all adjacent tiles,
   and confirm no node handle, pose, audio or physics binding changes.
3. Teleport, move quickly and spin the camera; assert no missing nearby
   required tile, stale generation, use-after-free or GPU descriptor hazard.
4. Cook twice with identical inputs, compare cooked tile/BVH payloads;
   verify no per-load static BVH construction after format cutover.
5. Repeat with no scene content, corrupt/missing tile pages, low memory,
   cross-cell geometry, prefabs and rapid cancellation/re-request.
6. Compare performance to baseline and assert the scene visibility pass
   scales with traversed/visible candidates, not all authored instances.

## What this initial PR actually changes

Epoch 1's standalone CPU streaming seam (including ticket-safe completion),
the preliminary Map/tile codec slice of Epoch 2, and an SMP8 tile-page
read/write adapter slice of Epoch 3 are implemented with headless tests.
**Epochs 2–7 are not claimed complete.** Do not merge this
as the requested finished architecture merely because the PR exists.
