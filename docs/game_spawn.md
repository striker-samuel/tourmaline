# Game-code spawning (first ergonomic slice)

Include `<Engine/App/game_spawn.h>` and create a short-lived `tour::game::Spawner` from the live `tour::World`.

```cpp
tour::game::Spawner spawn(world);

// Resolve these world-row indices from your typed asset handles immediately
// before spawning. They are NOT stable asset identities across content changes.
auto enemy = spawn.Mesh(meshWorldIndex)
    .Material(materialWorldIndex)
    .At(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)))
    .Spawn();

auto prop = spawn.MeshFromNode(authoredNodeId).Spawn();

SceneAudioSource source{};
source.volume = 0.5f;
auto emitter = spawn.Audio(source).Spawn();

// Handles are stable registry identities; don't cache MeshEntityNode()
world.DestroyMesh(enemy);
world.DestroyMesh(prop);
world.DestroyAudioSource(emitter);
```

`Mesh(...)` and `MeshFromNode(...)` return `smvk::MeshEntityHandle`, and `Audio(...)` returns `smvk::AudioSourceHandle`. A failed spawn returns an empty handle. Requests hold a reference to the World; do not keep them after World destruction. The audio request copies its definition.

## Deliberate constraints

- These are **entity spawning** helpers, not a new scene-storage implementation. They use the existing world spawn functions and retain their current lifecycle semantics.
- `Mesh(worldMeshIndex)` expects a current World mesh-location index, not a persistent asset id. Use the asset manager to resolve a stable typed asset key on each spawn.
- No `Under(parent)` operation yet: existing mesh/audio spawn implementations ignore their parent argument. Parenting must become real and survive cluster changes before we expose it here.
- No generic `Node()`, `AttachMesh()`, `AttachLight()`, or node-destruction API yet. These require the incremental World-owned node work (Epoch 6c) and dynamic-cluster retirement (6d) documented in `docs/scene_tables_migration.md`.
- Transform and component changes on spawned entities are currently performed through the World scene / entity methods, with their existing node-remap restrictions. Do not keep raw `NodeId` values across a full cluster resolve.
- The convenience header does not change the renderer, editor, asset ownership, or serialized format.

## Intended follow-up shape (not implemented)

The longer-term game API should look like this once World nodes are authoritative:

```cpp
auto entity = world.SpawnNode({ .name = "Enemy", .parent = root });
entity.SetLocalTransform(frame);
entity.SetMesh(meshAsset);
entity.SetMaterial(materialAsset);
entity.AddAudio(source);
entity.Destroy();
```

The stable **node handle** should be the durable identity, and mesh/material attachments should resolve through typed asset handles rather than ephemeral location indices. Parenting, removal and reuse must validate generations; unloading a content cluster should retire only its nodes, not game-created nodes.
