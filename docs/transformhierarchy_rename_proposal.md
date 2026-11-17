# Rename the flat node-transform tree (`Scene` → `TransformHierarchy`)

Status: **proposal / draft — NOT implemented.** This records the rename, the
rationale, the scope guardrails, and the touched-surface estimate so the
mechanical pass can be reviewed and scheduled as one deliberate change
rather than an ad-hoc sweep.

## Why this rename is being asked for

`tour::scene::Scene` names the **flat node-transform tree**: the slot-indexed
`hierarchy` (parent/child/sibling links), `localTransform` / `globalTransform`,
node generations/free-list, the typed association `tables` (meshes, materials,
lights, cameras, decals, …), and the scene-level environment/render settings.

The word "scene" is overloaded and misleading here. The same codebase uses
"scene" for at least four *different* concepts that have nothing to do with the
flat node arrays:

1. the scene **asset** (`.tour` / `.gltf` / `sceneNames` / `sceneIndex`),
2. the scene **pak blob kind** (`PakAssetKind::Scene`),
3. the scene **instances** (`ProjectScenes`, `LoadScene`),
4. the scene **name** / discovery (`ListPakScenes`, `PakSceneInfo`).

So `WorldPage::scene` (a node tree) reads like "the world page's scene asset",
which has repeatedly caused ambiguity during the multi-page world work. The
rename pins the node-transform-tree concept under one unambiguous name.

## Proposal

| From | To |
| --- | --- |
| namespace `tour::scene` | namespace `tour::transformhierarchy` |
| `struct Scene` | `struct TransformHierarchy` |
| `struct Hierarchy` (parent/child/sibling links) | `struct NodeLinks` |

The `smvk::` compat re-exports follow the landed migration pattern: add
`smvk::transformhierarchy` aliases for the namespace family, keep consumers
compiling, migrate call sites opportunistically, then retire the `tour::scene`
spelling.

## Explicitly out of scope (do NOT touch)

These "scene" names are a *different* concept and stay as-is:

- `PakAssetKind::Scene` (the pak blob kind) and every `::Scene` enum value
  (`StreamLayer::Scene`, etc.).
- `ProjectScenes` / `LoadScene` / `LoadSceneAdditive` (scene *instances*).
- Scene *asset* strings and symbols: `sceneNames`, `sceneIndex`, `scenePak`,
  `sceneName`, the `.tour` / `.gltf` paths.
- The compound node-local payload types `SceneLight`, `SceneCamera`,
  `SceneDecal`, `SceneRenderSettings`, `SceneTables`, `SceneEnv`, and their
  `Scene*` siblings (a possible follow-up: `NodeLight` / `NodeCamera` /
  `NodeDecal` / `RenderSettings` — **not** part of this pass).
- `Skeleton` and the skinning bone hierarchy (a completely separate
  hierarchy — the whole point of the guardrail).
- The member *variable* named `scene` (`WorldPage::scene`, `World::scene`,
  `cluster.scene`, `world.scene`, …). This is a much larger mechanical pass
  (thousands of `.`-access sites) and is deliberately a separate, later
  change — the type/namespace rename does not force it.

## Risks

1. **Mechanical breadth.** `\bScene\b` appears in ~175 files and
   `tour::scene::` in ~55 files; `\bHierarchy\b` in ~13. The pass is the same
   shape as the earlier `Cluster` → `WorldPage` rename (112 files).
2. **Word-boundary hazards.** A blind `Scene` → `TransformHierarchy` replace
   must NOT match `PakAssetKind::Scene` / `StreamLayer::Scene` (enum values)
   or any `SceneXxx` compound (`SceneLight`, `SceneTables`, `ProjectScenes`,
   `PakSceneInfo`). The safe order is: protect `::Scene` enum values and
   compound names, replace the standalone type, restore.
3. **`Hierarchy` name collision.** After the rename, `TransformHierarchy`
   must not absorb the `Hierarchy` → `NodeLinks` replacement (order matters:
   rename `Hierarchy` → `NodeLinks` *before* `Scene` → `TransformHierarchy`,
   or use a boundary-aware pass).
4. **Skeleton bone hierarchy.** `Skeleton` / skinning code uses the word
   "hierarchy" for the bone tree; it must be left untouched — the rename
   targets only `tour::scene::Hierarchy` / `Scene`.
5. **Compat re-export baselines.** The R7/R9 architecture gates track exact
   per-file compat-alias counts. Adding the `smvk::transformhierarchy` aliases
   (and later retiring the `tour::scene` spelling) is a deliberate R7/R9
   baseline row in the same commit — growth or shrinkage both fail otherwise.
6. **Serialization ABI.** None — this is a source-level type/namespace rename;
   the scene blob layout and the pak magic are untouched.

## Files touched (estimate)

- **Definition:** `engine/include/Engine/Scene/scene.h` (+ `scene_node_id.h`,
  `scene_tables.h`, `scene_payloads.h`, `static_export.h`, `Engine/Utils/bvh.h`
  scene helpers), `engine/src/Scene/Scene.cpp` (and `SceneTables.cpp`).
- **Type consumers:** ~175 files across `engine/`, `app/`, `tools/`, `editor/`,
  `tests/` referencing the `Scene` type.
- **Namespace consumers:** ~55 files referencing `tour::scene::`.
- **Links struct:** ~13 files referencing the standalone `Hierarchy` type.

## Rollout

1. Rename the definition + add `smvk::transformhierarchy` compat aliases
   (R7/R9 baseline rows in the same commit).
2. Migrate `tour::scene::` call sites to `tour::transformhierarchy::`.
3. Migrate the `Scene` type to `TransformHierarchy` and `Hierarchy` to
   `NodeLinks` with the enum-value protections above.
4. Build + `ctest` (the compile is the verification — no behavior change).
5. Retire the old `tour::scene` spelling (delete the alias + shrink the R7/R9
   baselines in the same commit).

The separate, larger `scene` member-variable rename (`WorldPage::scene` →
`WorldPage::nodes` or similar) is intentionally deferred.
