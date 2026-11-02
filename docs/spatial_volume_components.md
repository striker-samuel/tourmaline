# Spatial Volume components and the Component Spreadsheet

Spatial Volume is a scene transform with a spatial box: global translation
is its center; normalized global basis columns are its axes; half their lengths
are half-extents. See `Engine/Scene/spatial_volume.h`.

## Authoring

1. **Create → Spatial Volume** adds a named 8 × 4 × 8 m box near the camera target.
   Translate, rotate, or scale its transform to shape it.
2. The Inspector lets you **Add/Remove GI Volume**, **Audio Room**, or **Trigger**
   components independently. Several behaviors may share the same volume.
3. **View → Component Spreadsheet…** opens a Houdini-style table: one instance
   per row; properties across columns. Select a component type, sort or filter
   rows, resize/reorder/hide columns, and press Enter to commit edited cells.
   Checkboxes commit immediately. Undo/redo uses the shared editor commands.
   Transform lists all live nodes; Light, Camera, Cubemap, Audio Source,
   GI Volume, Audio Room, and Trigger Volume have typed property tables.

The `.tour` file stores generic transform creates and subsequent component
association/attribute verbs. New transforms can be referenced by
`-ref <index> -owner transform`. Baked GI artifacts may reference this
same transform family. Older scene versions are not migrated.

## Runtime Trigger

`SceneTriggerVolume` holds enabled, once and a text tag. Trigger rows survive
pak serialization and world scene merging. Game code supplies actor positions
to `tour::scene::TriggerVolumeTracker::Update` once per simulation tick.

```cpp
tour::scene::TriggerVolumeTracker tracker;
tracker.Update(world.scene, actorId, actorWorldPosition,
    [&](const tour::scene::TriggerEvent& event) {
        // event.type: Enter or Exit
        // event.node: volume node
        // event.tag: authored identifier
    });
```

Once suppresses reentry events after the actor's first visit. Call
`ResetActor(actorId)` when an actor is destroyed and `Reset()` on world unload.
This is a reusable API; the App does not automatically call it for actors.

Audio rooms remain listener-scoped environmental reverb on the world-SFX bus;
this PR does not implement audio portals or per-source room occlusion.
