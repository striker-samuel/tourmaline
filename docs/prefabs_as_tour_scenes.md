# Prefabs are `.tour` scenes

Tourmaline does **not** introduce a `.prefab` format, asset kind, or
independent prefab serializer. A reusable prefab is an ordinary, authored
`.tour` scene with its normal source references, node commands and
per-occurrence overrides. It uses the existing `project::scenes::* SceneKey`
identities, cooked scene blobs, pak ownership, and cluster lifecycle.

## Authoring: scene references

The existing `reference` verb already nests a complete `.tour` scene,
recurses into its own references (cycle guarded), and namespaces each
occurrence. A scene can reference the **same** scene as many times as it
needs, without modifying that source.

```text
reference -root -n 0 -source "content/levels/arena.geometry";
reference -n 0 -source "content/pieces/crate.tour" -name "crate_a"
    -t 10 0 0 0 90 0 -s 1 1 1;
reference -n 0 -source "content/pieces/crate.tour" -name "crate_b"
    -t -10 0 0 0 180 0 -s 2 2 2;
```

Each command is one line in a real `.tour` file (the line wrapping above
is illustrative). The authored reference syntax now accepts:

- `-t x y z`: translation (existing behavior).
- `-t x y z rx ry rz`: translation + Euler XYZ rotation **in degrees**.
- `-s sx sy sz`: component scale (defaults to 1,1,1).
- The placeholder matrix is `T * Rx * Ry * Rz * S`. The reference's
  actual scene nodes remain local to the referenced definition.
- Optional `-namespace "vehicle"` overrides the prefix for this reference's imported nodes and nested references (`vehicle:Wheel`, `vehicle:gear:Gear`), independent of the placeholder `-name`. Without it, the namespace defaults to `-name` for compatibility. Namespace values must be nonempty, contain no `/` or `:`, and apply only to non-root scene references.
- Existing `-name`, `-guid`, `-enabled`, and nested reference behavior
  stay unchanged. The `-root` reference is the definition's own
  geometry source and does not become an extra placeholder.

The editor's *Add scene reference* flow and reference panel remain the
authoring entry points. The reference placeholder is a real scene node;
edits to that occurrence stay in the owning scene's `.tour` overlay.
No new per-prefab component language or automatic property inheritance is
introduced.

### The pak lane rejects scene references

Scene references are an **editor-side authoring composition** (bound at
import by `ApplySceneReferences`, editor parity). They are **not a
shippable content form**: the recipe resolver and the pak cooker both
REJECT a scene whose references name another `.tour` (root or not) —
a pak cooks ONE flat scene and never silently flattens a scene graph
the author never sees flattened. A scene destined for a pak duplicates
the model artifact directly (the `source -n <parent> -p "<file>.geometry"
-name "<n>" -t x y z` verb); the *shipping* repeated-instance mechanism
is the runtime `InstantiatePrefab` lane below (separately cooked scene,
instantiated by stable scene content ID).

## Runtime: instantiate a cooked `.tour` scene

Scenes already support repeated additive loading and generational
`SceneHandle`s. `ProjectScenes::InstantiatePrefab` adds an initial
world transform to a distinct instance of a `project::scenes` asset:

```cpp
std::string err;
glm::mat4 position = glm::mat4(1.0f);
position[3] = glm::vec4(4, 0, 2, 1);

app::SceneHandle crate = scenes.InstantiatePrefab(
    world, ctx, streamer, project::scenes::Crate, position, err);
if (!crate) { /* report err */ }

glm::mat4 moved = glm::mat4(1.0f);
moved[3] = glm::vec4(8, 0, 2, 1);
scenes.SetPrefabTransform(crate, world, moved, err);

scenes.RemoveScene(crate, world, err);
```

- The source scene is loaded by **stable scene content ID**, via the
  existing pak, content registry and cluster loader. `InstantiatePrefab`
  does not read a raw `.tour` file at runtime.
- The instance frame pre-multiplies the cooked scene root's original
  local transform. Every child retains its authored local placement.
- `SetPrefabTransform` is **absolute**, not cumulative: setting the
  same pose repeatedly has the same result. Only that instance's root
  is changed; the original cooked asset and other instances stay intact.
- `PrefabTransform(handle)` queries the placement frame for live prefab
  instances. Ordinary additive scenes are *not* writable via this API.
- `RemoveScene` is the destruction path, preserving existing asset
  ownership, generational stale-handle checks and release behavior.
- Nonfinite matrices are rejected before scene loading or mutation.
- Loading, moving and removal run on the main/update thread because the
  world scene is fully resolved/merged. **Not** a per-frame way to
  move thousands of crates.

## Limitations and next steps

This deliberately reuses **cluster-per-instance** scene loading. It is
appropriate for authored assemblies (buildings, doors with children,
interactive set pieces) but is **not** an efficient replacement for
instancing thousands of identical mesh entities. The existing
`AssetManager::SpawnMesh` path remains preferable for fine-grained
mass spawning.

Potential follow-ups: a scene-template resource cache with independent
per-instance transform/visibility tables, editor reference-instance
rotation/scale widgets, true runtime per-occurrence property overrides,
named attachment anchors and incremental world-root transform updates.
None should create a second prefab serialization system.

Tests: `TourTests` for `.tour` reference TRS and nested occurrences,
`PakHostTests` / `TestProjectScenes` for isolated runtime instances,
absolute placement, removal, and stale handles.
