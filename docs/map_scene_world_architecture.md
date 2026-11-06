# Map, Scene, and runtime World: distinct authoring and ownership domains

## Decision

`tour::Map` is the **world-wide authored document**. It may contain its own
hierarchy of organizational anchors and placed-scene references. Those nodes
are NOT copies of the local scene hierarchies.

`tour::scene::Scene` is the **local authored scene document** for an interior,
prefab, reusable building, or other contained hierarchy. It is not a world
partition, pak, map, or global spatial index.

`tour::World` remains the **runtime simulation/rendering host** with stable
world-qualified entity identities, resident scene instances, GPU scene data and
dynamic entities. One Map may result in many streamed runtime instances; one
local Scene asset may be instantiated many times.

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

## Stable identifiers

- Map nodes: durable authored `uint64_t` IDs, not array offsets.
- Scene assets: durable project content IDs; multiple Map nodes may reference
  one scene asset.
- Runtime scene instances: world-qualified generational handles.
- Runtime nodes: world-qualified generational identities, not source-map IDs.
- Cooked BVH primitives: local cooked instance keys stable across remap.
- World tiles: `WorldCellKey` logical coordinates, not archive filenames.

## Implemented stepping stone

`engine/include/Engine/WorldPartition/WorldMap.h` defines a separate
`tour::Map` with a node hierarchy, optional placed Scene content ID and a
deterministic `CompileMapTilePlacements` phase. It handles nested transforms,
cycle/ID validation, persistent placements and canonical root-cell ownership.

This is an in-memory cook preparation API only. It has NOT been connected to
the editor `.tour` document format, Project Build, the pak writer, the
runtime loader or the GPU scene. It does NOT yet compute conservative bounds
of a referenced Scene or cross-tile spatial visibility. Those must precede
runtime use.

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
- Make Project Build cook Map and Scene artifacts independently.
- Serialize tile placement manifest plus spatial BVH pages into SMP8.
- Add stream provider IO/dependencies and activate instances incrementally.
- Preserve standalone scene/prefab compatibility across the cutover.
