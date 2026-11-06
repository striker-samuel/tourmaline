# Catalog-first World node API — contract for Epoch 6c/6d

> **Design and regression checkpoint, not a delivered gameplay API.**
> The current World cluster merge rebuilds the scene and changes node
> identities; this API must not be exposed as durable until Epoch 6c
> replaces that destructive merge. PR #235 audits that prerequisite.

## Separate identities

- **World nodes** are runtime identities. Each node owns **one transform**
  and has **zero or one mesh association**. Nodes may exist without assets.
- **The generated project catalog** owns asset identity. Game code passes
  typed keys such as `project::meshes::Stone`, never transient World mesh
  location indices. AssetManager/AssetStore own loaded resource lifetimes.
- **Value components** are inline definitions, not catalog references.
  They can be assigned directly using default initialization `{}` or
  an explicit payload. The node's transform places those components.
- Reusing the same catalog mesh on many nodes shares the resource, **not**
  the node. `SetMesh` **replaces** the existing binding; it never adds a
  second mesh, silently creates a child, or changes the node's transform.

## Desired C++ game-code syntax (not implemented)

```cpp
auto node = world.AddNode();
world[node].SetLocalTransform(frame);
world[node].SetMesh(project::meshes::Stone);

// A different catalog key REPLACES the same mesh slot.
world[node].SetMesh(project::meshes::Granite);

// Typed value components: no catalog lookup, defaults are constexpr-capable
// where their value types permit it.
world[node].SetPhysicsBody({});
world[node].SetAudioRoom({});
world[node].SetTriggerVolume({});

// Alternatively, specify the type explicitly for generic operations:
world[node].SetPhysicsBody(ScenePhysicsBody{ .mass = 2.0f });
```

For the user-facing shorthand `Mesh::Stone`, the generated project
catalog may provide an alias or namespace; its real generated spelling
currently follows `project::meshes::Stone`. Do not invent fallback
asset names, hashes, or string lookup in the World.

A generic `SetType<T>(T)` cannot infer `T` from an empty `{}` in C++.
Use a concrete typed setter `SetPhysicsBody(const ScenePhysicsBody&)`,
`SetAudioRoom(const SceneAudioRoom&)`, etc. No key lookup is involved;
the compiler knows the payload type from the setter signature.
`SceneTriggerVolume` includes a string and is a value payload but is
**not** a trivially-copyable POD. Do not falsely require every inline
component to be a literal/constexpr type.

## Required semantics

1. `AddNode` allocates one world-owned stable node/transform, and returns
   a **generational NodeHandle**. Existing content nodes and live handles
   remain valid through content load/unload.
2. `world[node]` validates that handle before any mutation; a deleted and
   reused slot cannot be controlled through a stale handle.
3. `SetMesh(key)` validates and acquires the catalog resource **before**
   replacing the current mesh; on failure, existing bindings stay intact.
   A successful replacement retains the same node/transform/children and
   releases the old resource only once it has no remaining owners.
4. The mesh binding is a **single slot per node** (the typed scene-table
   `SetMesh` already provides replacement semantics). Any renderer
   cache/draw journal must be updated when that slot changes.
5. For inline value components, a setter accepts `{}` directly. Each
   kind has one replaceable slot. Components sharing a node reuse its
   **one transform**; they do not create extra transform nodes.
6. Node deletion removes associations once, including mesh asset leases,
   and invalidates its handle; parenting and unloading preserve the
   hierarchy contract.

## Current verified foundation

The existing scene-table functions `SetMesh(Scene&, NodeId, index)`,
`SetPhysicsBody(Scene&, NodeId, ScenePhysicsBody)`,
`SetAudioRoom(Scene&, NodeId, SceneAudioRoom)`, and
`SetTriggerVolume(Scene&, NodeId, SceneTriggerVolume)` already operate
on one association per kind with replacement semantics. The
`scene_table_tests` additions pin mesh replacement without moving a node
and default-braced value components.

**Not implemented:** `World::AddNode`, `World::operator[]`, direct
catalog attachment to world nodes, incremental cluster streaming (6c),
and dynamic-cluster retirement (6d). Do not merge this checkpoint as a
finished implementation.
