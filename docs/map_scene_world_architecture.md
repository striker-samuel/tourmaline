# Map, Scene, and runtime World: distinct authoring and ownership domains

## Decision

`tour::Map` is the **world-wide authored document**. It may contain its own
hierarchy of organizational anchors and placed-scene references. Those nodes
are NOT copies of the local scene hierarchies.

`tour::scene::Scene` is the **local authored scene document** for an interior,
prefab, reusable building, or other contained hierarchy. It is not a world
partition, pak, map, or global spatial index.

`tour::World` (the renderer's `m_world`) remains the **single active runtime
simulation/rendering host**. It owns active scene instances and dynamic
entities. Many authored Maps can exist on disk/in the project,
but **exactly one Map is active in the runtime at a time**. A Map may place
many local Scenes; each Scene asset may be reused by many placements.

```
Map (world document)
  terrain/regions and authored global environment
  anchor hierarchy
    village anchor
      place SceneAsset(church) at transform
      place SceneAsset(crypt) at transform
    persistent player spawn
  tile ownership determined by cook

Scene (reusable local hierarchy)
  root
    walls
    furniture
    lights
    components

Project Build
  Map -> tile ownership manifest + global persistent placements
  Scene -> local scene blob + local static BVH
  Tile -> references to local Scene asset IDs + instance transforms
  Pak -> positional paged archive; NOT spatial ownership

WorldPartition -> tile demand -> asset pages -> World instance activation
```

## World asset identity is not a second runtime World

- `WorldId` identifies an authored Map/world-wide spatial distribution.
  It is stable across recooks and not derived from the pak pathname.
- `WorldTileKey { WorldId, WorldCellKey }` identifies a tile within an
  authored Map, so two Maps can both contain tile `(0, 0)`.
- `tour::World` is the runtime host, not another authored world. Its active
  Map ID selects one streamed distribution. A map switch retires old tile
  ownership before installing the next manifest; streaming interests from
  the old Map must not activate the new Map's tiles.
- Do not create additional runtime-World instances, simultaneous active Maps,
  or an unnecessary runtime-world-instance ID. Existing generational entity
  handles remain runtime state and must invalidate safely on a Map switch.
- The initial `SceneTileProvider` enforces single-Map manifest ownership;
  production `m_world` switch/unload integration remains outstanding.

## Stable identifiers

- Map nodes: durable authored `uint64_t` IDs, not array offsets.
- Scene assets: durable project content IDs; multiple Map nodes may reference
  one scene asset.
- Runtime scene instances: world-qualified generational handles.
- Runtime nodes: world-qualified generational identities, not source-map IDs.
- Cooked BVH primitives: local cooked instance keys stable across remap.
- World tiles: `(WorldId, WorldCellKey)` logical identity, not archive filenames.

## Implemented stepping stone

`engine/include/Engine/WorldPartition/WorldMap.h` defines a separate
`tour::Map` with a node hierarchy, optional placed Scene content ID and a
deterministic `CompileMapTilePlacements` phase. It handles nested transforms,
cycle/ID validation, persistent placements and canonical root-cell ownership.

The `.map` text reader, standalone `pak --map` mode, project-recipe
`map <path>` directive, shared Project Build executor, and SMP8 MapTile pages
now provide an end-to-end **format/packaging** path. MapTile TOC discovery
selects one WorldId without loading other Maps' pages. Persistent placements
are separate pages so they do not evict with camera-facing tiles.

`app/src/MapSceneStreaming.h` is a production-API bridge that reads tile
pages and instantiates their local Scene references through existing
`ProjectScenes::InstantiatePrefab` / `RemoveScene`. It is available to
hosts via `WorldRenderer::StreamingPartition()`, `SetActiveMapId()`, and
`SetMapStreamingRadius()`; the host still has to own/register the bridge
after renderer attachment and unbind it before provider destruction.

**Incomplete:** the bridge still uses Cluster-based additive scene loading;
it does NOT yet give incremental world-node mutation, background paging,
independent asset-store ownership, spatial BVHs or visibility-driven GPU draws.
The separate authored Map editor/persistence UI has not been implemented.

## Critical correctness rules

1. Tile owner is selected deterministically for each placement; large scenes
   overlapping several cells receive separate **visibility** references,
   never copies of authoritative node/entity ownership.
2. Local scenes can exist without a Map and can be loaded standalone.
3. Moving a placement changes its Map transform, not its original Scene.
4. Editing an instance override is explicit; modifying the source Scene
   updates all instances on recook/reimport under editor policy.
5. Streaming out a tile does not destroy promoted persistent runtime entities,
   and retaining a shared Scene asset does not retain its retired instances.
6. Map editor undo/reimport uses Map authored IDs; Scene editor undo/reimport
   uses Scene authored IDs; neither assumes runtime node slot identity.
7. Do not replace `tour::World` with a second mutable `Map::scene`. Map is
   an authored/world-cook specification, not a second simulation state.
8. Interior Scene local BVHs are reusable when instances move/rotate/scale.
   Global tile coarse bounds must be conservative across placed transforms.

## Future gates

- Wire Map persistence and editor placement UI.
- Complete the Project Build's static BVH and full dependency derivation for
  independently cooked local Scenes. (Map placement-page build is wired.)
- Serialize tile placement manifest plus spatial BVH pages into SMP8.
- Replace the synchronous legacy ProjectScenes bridge with budgeted
  asynchronous IO + incremental World publication.
- Preserve standalone scene/prefab compatibility across the cutover.
