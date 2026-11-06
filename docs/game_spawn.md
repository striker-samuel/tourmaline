# Game nodes and catalog-backed mesh assignment

The World owns nodes and their transforms. The generated project catalog
names assets; AssetManager loads those identities into reusable resource
handles. **No separate CatalogWorld object** and no raw world resource
indices in gameplay code.

## Game-code API (implemented in this branch)

```cpp
#include <Engine/App/world.h>
#include "AssetManager.h"
#include "project.h"       // generated, project-specific catalog

// Construct AssetManager once using the SAME world; the manager
// automatically attaches to world for its lifetime.
app::AssetManager assets(runtime, world, ctx, &renderer);

// Load from a typed catalog key BEFORE creating nodes. The loaded
// handle is a RESOURCE, not a node or a runtime mesh entity.
app::MeshHandle stone = world.LoadMesh(project::meshes::Stone);
if (!stone)
    throw std::runtime_error(world.LastAssetError());

const NodeHandle node = world.AddNode();
world[node].SetLocalTransform(transform);
world[node].SetMesh(stone);        // one optional mesh slot per node
world[node].SetPhysicsBody({});   // inline value (no catalog lookup)
world[node].SetAudioRoom({});
world[node].SetTriggerVolume({});

// A different loaded mesh REPLACES the existing slot on this same
// node, preserving its transform and other component associations.
// auto granite = world.LoadMesh(project::meshes::Granite);
// world[node].SetMesh(granite);

// Unload in reverse: detach the node before releasing its resource.
world[node].RemoveMesh();
world.RemoveNode(node);
assets.UnloadMesh(stone);
```

All mesh resources come from `World::LoadMesh(key)` (delegating to the
attached `AssetManager::LoadMesh`) or the existing
`AssetManager::LoadMesh(key, error)` overload. The returned
`app::MeshHandle` is shared across nodes and is passed explicitly to
`world[node].SetMesh(handle)`; **do not pass a catalog key directly to
`SetMesh`**. For diagnostic code,
`World::LoadMesh(key, std::string& error)` is also available.

`world.AddNode(frame)` returns a generational `NodeHandle`.
`world[node]` is a lightweight proxy borrowing the World, which
validates the handle before writes. `SetMesh(handle)` uses the
manager's current World mapping and the scene's single mesh row per
node; it refuses invalid/unmapped handles without replacing an
existing association. `RemoveMesh()` removes only that row.
`RemoveNode(handle)` destroys the game-owned node and its inline rows.

Concrete component setters such as `SetPhysicsBody({})`,
`SetAudioRoom({})`, and `SetTriggerVolume({})` accept default
braced values without a catalog key or template argument. Some inline
value components contain `std::string`, and are not PODs or constexpr
values. These are *typed value definitions*, not loadable assets.

## Runtime invariants and resource lifetime

- One node has **one transform** and **zero or one mesh**. Multiple
  nodes may bind the same loaded mesh handle, with independent transforms.
- `SetMesh` replaces the mesh on the **existing** node; no second node
  or hidden transform is created.
- The loaded asset belongs to `AssetManager`. The node merely borrows
  its handle. `UnloadMesh(handle)` refuses while any game node still
  refers to it. `UnloadAllMeshes()` and AssetManager detachment first
  clear game-node mesh associations so resources cannot dangle.
- The same World must outlive its AssetManager. Game code should
  detach/destroy nodes and release resources before teardown.
- Unknown catalog keys, wrong catalog kinds and missing World mappings
  fail via existing AssetManager checks. No fallback string lookup
  or fake resource index is introduced.

## Known limit: legacy streaming/cluster resolve

**Epoch 6c is not complete.** The current legacy
`ResolveAndFixup_clusters()` reconstructs the World scene. It
invalidates game node handles and bindings created through
`AddNode()`; the implementation explicitly clears registrations and
preserves a high-water slot generation to guard against stale handles
silently becoming live again. Thus, during this interim API, **load
mesh assets before creating gameplay nodes**, and do not expect those
nodes to survive another legacy cluster load/unload or full resolve.

That is a known functional limitation to remove in the 6c incremental
World-node migration, not a claimed implementation of #235's complete
dynamic-cluster retirement. The other catalog resource kinds still use
their existing specialized loaders/services; no generic
`world[node].SetAsset(anyKey)` exists yet.

Tests added in this branch cover typed inline components, node
creation/deletion and handle invalidation, one-mesh replacement,
and catalog-key -> loaded mesh handle -> node attachment through the
apphost's CPU-only AssetManager seam.
