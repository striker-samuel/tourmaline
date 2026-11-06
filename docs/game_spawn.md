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

// The app host creates World and its internal asset service during
// World loading. Game code does NOT construct an AssetManager.
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
world.UnloadMesh(stone);
```

All mesh resources come from `World::LoadMesh(key)`, delegating to
the runtime-owned AssetManager for catalog lookup, caching and lifetime
management. The game needs no access to that internal service. The returned
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

## Catalog-driven artistic LUTs

LUTs no longer use a renderer-side directory scan or a filename-based
loader. The project build owns the `luts/` content and generates
`project::luts::<Symbol>` as a distinct **typed view** of the existing
project texture catalog. The stable ID is that texture's original ID;
there is **no second registry or runtime string lookup**.

Add a `content/luts/neutral.png` (or another file under a `luts/`
folder) to the project's recipe asset set, then regenerate and cook the
project. The cook preserves its PNG strip bytes rather than transcoding
it into a normal KTX2 material texture. LUTs must be **1024x32 PNG
strips** (32³ grid, sRGB encoded) for the current tonemap pass.

```cpp
auto grade = world.LoadLut(project::luts::Neutral);
if (!grade)
    throw std::runtime_error(world.LastAssetError());

world.SetLut(grade);     // activates the loaded GPU color LUT
// ... when no longer needed:
world.SetLut({});        // disables color grading
world.UnloadLut(grade);  // retires the loaded resource handle
```

The loading and unloading pattern matches meshes: catalog key ->
loaded, generational resource handle -> World use -> World unload.
Unlike a mesh, a color LUT is a **global postprocessing state**, so
`world.SetLut(handle)` is on the World, not on `world[node]`.
`World::SetLut({})` disables grading; unloading the active handle
also disables it. A failed load never replaces an already active LUT.

Neutral LUT authoring/export to `exports/lut_neutral.png` remains a
tooling operation; engine startup no longer scans `content/luts`
or creates `content/luts/neutral.png` implicitly.

## Runtime invariants and resource lifetime

- One node has **one transform** and **zero or one mesh**. Multiple
  nodes may bind the same loaded mesh handle, with independent transforms.
- `SetMesh` replaces the mesh on the **existing** node; no second node
  or hidden transform is created.
- The loaded asset belongs to the runtime-owned AssetManager. The node
  merely borrows its handle. `world.UnloadMesh(handle)` refuses while
  any game node still refers to it; `UnloadAllMeshes()` at internal
  teardown first clears node mesh associations so resources cannot
  dangle.
- The World owns the service through an erased pointer installed by
  PakApp's World-source callback; the service is destroyed before the
  World's scene and cluster pools. No explicit service initialization
  or shutdown belongs in gameplay code. Headless/custom World
  constructors must still install a service explicitly if they want
  catalog-backed loading.
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
