# Authored Map and tile streaming — integrator quickstart

This is the end-to-end **Map packaging + transitional runtime bridge** path,
NOT the final cluster-free/GPU-driven implementation. The new Map describes a
world-wide spatial distribution; one Map is active inside one `tour::World`.

## Authored .map

```text
map 1 101
# node stableId parentStableId sceneContentId persistent tx ty tz
node 1 0 0 0 0 0 0
node 2 1 9001 0 0 0 0
node 3 1 9002 0 256 0 0
node 4 1 9003 1 0 0 0
```

The parent ID is an authored ID, not an array index. A scene ID of 0 is
an organizational node. Persistent placements load outside tile demand.
For rotations/scales, use a `matrix` row with 16 finite column-major doubles
instead of the final XYZ translation.

The stable Map ID (101 above) is independent of its filename/pak. Cooked
tile identity is `WorldTileKey{WorldId, WorldCellKey}`.

## Standalone packaging

```sh
pak <content-root> --map <authored-file.map> --out-pak <output.pak>
```

No local Scene or Vulkan device is needed. The packer emits a MapManifest page,
a persistent placement page when needed, and independently readable MapTile
pages. `--verify on` reopens the output file with PakReader::OpenFile
and verifies every tile page through the same runtime decoder.

Map pages may also be added to a pak with local .tour Scene content.

## Project Build

A recipe may contain a map-only package beside the usual entry Scene package:

```text
recipe 1
entry content/scenes/entry.tour
[pak entry]
output entry.pak
scene content/scenes/entry.tour
[pak map]
output map.pak
map content/maps/main.map
depends entry
```

The resolver passes the Map to the shared Project Build Pak stage, which
publishes it through the existing staging and manifest pipeline. The local
Scene assets named by numeric ID must still be cataloged in project content.

## Host runtime bridge

`app/src/MapSceneStreaming.h` exposes a MapSceneStreaming owner. The host
constructs it with ProjectScenes, its current `tour::World`, GPU upload
context, and texture streamer. With a live `PakReader`, it calls
`BindMap(reader, worldId)`, then registers
`&bridge.Provider()` with `WorldRenderer::StreamingPartition()`, and
sets `WorldRenderer::SetActiveMapId(worldId)`.

The renderer injects a Scene-layer interest alongside WorldFields/Placement
every frame, with a configurable map streaming radius. The bridge uses
SMP8 tile-page reads and `InstantiatePrefab`/`RemoveScene`. To switch Maps:
stop producing demand, `UnbindMap()` and confirm success, reset the active
renderer Map ID, then bind/register the new Map. Keep the reader, ProjectScenes
and provider owner alive until WorldPartition detaches/clears borrowed
providers. The bridge is synchronous and not suitable for low-latency
streaming of many heavy scenes yet.

**Important:** the apphost has not been changed to automatically discover
which authored Map is active; selection and bridge lifetime still belong to
the host. An empty Map remains a valid active identity.

## Integrator gates

1. Compile all build targets, including pak, project pipeline, apphost,
   WorldRenderer and CTests. Header-only MapSceneStreaming is parsed in
   ProjectScenes.cpp to catch compile failures.
2. Run `ctest -N` then `ctest --output-on-failure` on the build tree.
   In particular, run `world_map_tests`, `scene_tile_provider_tests`,
   `project_pipeline_tests`, `pak_host_tests`, `world_entity_tests`,
   `world_partition_tests`, and `content_registry_tests` where present.
3. Pack the example Map and check its MapManifest, two independent tile
   pages and persistent placement with file-backed PakReader.
4. Test two authored WorldIds with overlapping (0,0) cells, zero tiles,
   missing/corrupt page, negative coordinates, and stale async ticket.
5. Run Project Build with a map-only pak and verify the published pak's
   MapManifest/MapTile pages through the content-addressed release.
6. With the runtime bridge, load adjacent cells, rotate quickly, unload,
   change active Map, and verify no scene handle accidentally survives or
   causes resurrection. Watch for synchronous hitches.
7. Do NOT treat passing this subset as completing Epochs 4–7. The final
   cutover requires independent asset lifetimes, incremental world identity,
   cook-time static BVHs, GPU instance streams, fence-safe retirement, and
   measured reduction of the reported 320 ms/frame floor.

No tests have been executed by this author.
