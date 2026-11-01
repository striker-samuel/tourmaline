# Content-as-Code: The Query, Loading, and GI-Blend API

**Status:** v1 implemented (the query API + the GI catalog lane + the
AssetManager facade + caller-controlled GI blending). The SHARED-ASSET
OWNERSHIP layer (`app::AssetStore` — the found-or-loaded carrier
acquire, the park-on-scene-removal bridge, the cluster-source seam),
the shared GPU geometry upload service
(`Engine/Renderer/geometry_upload.h`), and the TYPED-KEY SPAWN LANE
(`AssetManager::LoadMesh(project::meshes::X)` →
`AssetManager::SpawnMesh(handle)` → the engine's row-driven
`World::SpawnMesh(worldMeshIndex)`) are LANDED. The
subset/pak-grouped mesh fast path and the pak-vNext page pager remain
the documented follow-ups.

**Surface:**
- `engine/include/Engine/Asset/asset_query.h` — the compile-time
  query core (the regex engine + `MatchAssetsImpl`).
- `app/src/AssetStore.h/.cpp` — the shared-resource OWNERSHIP layer
  (the first slice of the consolidated content architecture; see
  its header for the semantic contract table).
- `app/src/AssetManager.h/.cpp` — the loading facade (composes the
  store; `Store()` exposes the cluster-source seam).
- `engine/include/Engine/Renderer/geometry_upload.h` — the shared GPU
  geometry upload service (the extracted carve + copy core; one
  uploader for the cluster deserializer, the future shared mesh
  store, and the pak-vNext pager — never a duplicate).
- The generated `project.h` — the per-project binding (emitted by
  `Project.exe ids`; never hand-edited).

## 1. The design principle

Application code **selects** assets at compile time, **loads** them
explicitly at runtime, and **controls interpolation** directly. The
engine never prescribes gameplay architecture, ownership of time, or
animation behavior — no engine-side transition durations, no easing
curves, no timers.

## 2. Compile-time queries: `project::MatchAssets`

The generated `project.h` binds a consteval query over its own
catalog. The pattern is a **regex** (regex semantics, not glob),
matched against the asset **symbol names** (the C++ identifiers the
catalog emits — the default) or the content **paths**
(`MatchField::Path`), filtered by the typed key's catalog kind:

```cpp
// Every mesh whose symbol starts with world_
inline constexpr auto WorldMeshes =
    project::MatchAssets<project::MeshKey, "^world_.*$">();

// Trees or rocks (alternation + groups)
inline constexpr auto WorldProps =
    project::MatchAssets<project::MeshKey, "^world_(tree|rock).*$">();

// UI textures by path
inline constexpr auto UITextures =
    project::MatchAssets<project::TextureKey,
        "^content/ui/.*$", ::project_pipeline::query::MatchField::Path>();
```

The result is a **precisely sized** `std::array<Key, N>` in catalog
order (deterministic — bucket order, id-sorted within). Zero matches
yield an empty array (`.data()`/`.size()` feed the batch loader
directly). **New assets matching a pattern join the result
automatically** at the next `project.h` regeneration + rebuild.

### The grammar

| Feature | Example | Meaning |
|---|---|---|
| Literals | `world_` | exact text |
| Wildcard | `.` | any character |
| Zero or more | `.*` | any sequence |
| One or more | `[0-9]+` | at least one digit |
| Optional | `(_lod)?` | optional suffix |
| Classes | `[abc]`, `[a-z]`, `[^0-9]` | listed / ranges / negated |
| Alternation | `tree\|rock` | either expression |
| Grouping | `(tree\|rock)` | group alternatives |
| Anchors | `^`, `$` | start / end of the subject |
| Escapes | `\.`, `\\`, `\n` | literal metacharacters |

**Match semantics:** the pattern must match the ENTIRE subject (a
full match — `bells` matches only the symbol `bells`; `^`/`$` are the
idiomatic explicit form). An empty pattern matches NOTHING (use
`.*`). Deliberately unsupported (asset-name matching needs none):
backreferences, lookahead/lookbehind, lazy quantifiers, POSIX
classes, `{m,n}` repetition.

A malformed pattern is a **compile-time error** (consteval
`static_assert` names the diagnostic; `PatternErrorText` lists the
grammar's error vocabulary). The engine compiles the pattern into a
bounded AST and matches via position-SET evaluation (the
Thompson/Pike NFA discipline) — linear in subject length, no
backtracking, no exponential paths.

### The emitted metadata

The generator emits, per project: the `AssetSymbols` table (the
symbol names in row order — compile-time query metadata, deliberately
OUTSIDE the runtime catalog ABI: `AssetDesc`, `CompiledAssetDesc`,
and the fingerprint folds are untouched), the `AssetCatalog` carrier
+ the `MatchAssets` binding, and the `AssetTraits<Key>`
specializations (the kind filter). `CatalogFingerprint` is UNCHANGED
by the query block — a compiled game and its deployed `.tapp` never
skew over it.

## 3. The AssetManager facade

```cpp
app::AssetManager assets(runtime, world, ctx, &renderer);

inline constexpr auto WorldMeshes =
    project::MatchAssets<project::MeshKey, "^world_.*$">();

auto batch = assets.LoadMeshes(WorldMeshes.data(), WorldMeshes.size());
```

- `LoadMesh(key, options)` / `LoadMeshes(const Key*, count, options)`
  — raw pointer + count (the borrowed-input convention: `nullptr` is
  valid only when count == 0; the manager never retains the input
  past the call). Repeated keys deduplicate; the per-pak grouping
  acquires each owning carrier once (never a re-carve). `MeshBatch`
  carries one handle per input key, in input order.
- **Resource handles stay separate from world entity handles**
  (loading never spawns entities). `MeshWorldIndex(handle)` resolves
  the handle's CURRENT world mesh row FRESH against the live
  `meshLocations` every call (a cached index is stale the moment any
  scene changes; a shared mesh can be carried by several scene
  instances, and a PARKED carrier has no row at all — UINT32_MAX is
  the honest no-current-mapping answer, never an error).
- **The typed-key spawn lane** (the gameplay surface, pinned by
  `meshent.rowspawn-*` + `am.mesh-spawn-*`):
  ```cpp
  auto tree = assets.LoadMesh(project::meshes::Tree);
  auto entity = assets.SpawnMesh(tree, spawnFrame, UINT32_MAX);
  // ... world.DestroyMesh(entity) when it dies.
  ```
  The manager resolves the handle's CURRENT row and calls the
  engine's row-driven `World::SpawnMesh(worldMeshIndex, frame,
  parent, worldMaterialIndex)` (the shared spawn core behind
  `SpawnMeshFromNode` — cross-cluster (owner, handle) binding, the
  resolve-tail re-mirror, the attached-animator cleanup all
  identical). A handle whose carrier is PARKED (its scene removed
  while the asset stays live) refuses with an empty handle — never a
  stale-row instance; the ASSET stays valid (the resource vs
  instance split).
- v1 mesh loading ACQUIRES the mesh's OWNING SCENE CLUSTER through
  **AssetStore** (the pak-format boundary: meshes ship inside their
  scene's cluster; materials + textures resolve exactly as
  scene loading). The join is the `[ids]` row's `.geometry` path =
  the pak's Mesh-blob URI prefix. A multi-scene pak whose scenes
  split one family refuses loudly (the documented v1 boundary).
- **The shared-asset retention contract** (the park bridge, pinned
  by the `am.mesh-*` + `wretain.*` checks in test_pak_host.cpp): the
  store holds asset-side references (`Cluster::assetRefs` +
  `World::RetainCluster`), so a `RemoveScene` whose carrier still has
  live handles PARKS it (`World::UnloadCluster` detaches it from the
  merged fold — the scene's nodes stop rendering, removal never
  resurrects scene content — while the mesh rows stay resident). The
  handle stays valid across the removal; a re-load shares the parked
  carrier (no re-carve of GPU ranges); the LAST reference releases
  it for destruction. `keepResident` (default) holds a sticky
  SESSION ref until `UnloadAllMeshes`/teardown, and residency
  upgrades propagate to the store (`MakeResident`) — the slot flag
  and the store's ledger stay in lockstep.
- The store's `clusterSource` seam (mirroring
  `ProjectScenes::ClusterLoader`) replaces the pak-to-cluster step
  for the GPU-less tests (hand-built mesh-carrying clusters stand in
  for the real deserialize); the default source is the engine's real
  deserialize, which refuses a null GPU context loudly.
- The carve + bulk-copy core behind the cluster's upload entry lives
  in the SHARED GEOMETRY UPLOAD SERVICE
  (`Engine/Renderer/geometry_upload.h`) — caller-owned arena tokens,
  pool-independent mesh values; the cluster's
  `UploadStaticMeshesDirect` is its cluster-bookkeeping side. The
  permanent model (page-resident mesh rows owned by the store, never
  by scene clusters), the subset/pak-grouped fast path, and the
  pak-vNext page pager remain the documented follow-ups. **Format
  policy for that work: when the paged format lands, the pak magic
  BUMPS — no version negotiation, no compat reader; stale paks fail
  loud at open (the existing `PakReader::Open` behavior) and content
  recooks from zero.**

## 4. GI as a first-class catalog asset

`GiKey` (the 0x6xxx range) names a **GI bake set** — a
`.givolumemanifest` (a baked lighting set naming its `.givolume`
payload rows). The resolver classifies `.givolumemanifest` as the GI
asset kind; the payloads + `.cubemanifest` stay bake plumbing. The
manifest bytes serve through the pak's VFS surface (the gi/ payload
channel), so `LoadGI` reads + parses them without a GPU.

```cpp
auto day = assets.LoadGI(project::gi::Day);
auto night = assets.LoadGI(project::gi::Night);
assets.SetGI(day);            // publish as the active set
assets.SetGIBlend(day, night, game.GetDayNightBlend());
```

Multiple sets stay resident simultaneously; `LoadGI` of the same key
returns the same handle (generational — stale handles fail).

## 5. Caller-controlled blending: `SetGIBlend(from, to, alpha)`

- `alpha` is clamped to `[0, 1]`: 0 = fully `from`, 1 = fully `to`,
  0.5 = the equal blend.
- The shaders **evaluate each set independently** — its own probe
  grids, volume layouts, vis layers, mean textures — then
  interpolate the RESULTING lighting (the diffuse indirect composite
  and the volumetric fog GI both).
- Both sets stay RESIDENT while blended: repeated `SetGIBlend` calls
  with the same handles only move the SCALAR (no rebuild, no
  re-upload, no re-upload per frame). Old GPU resources retire
  behind the in-flight frames through the deferred-destroy queue.
- The B set carries no tile masks (the cull hoist walks set A; the
  B walk's per-pixel AABB + plane weights own correctness) and no
  probe spheres (the debug overlay stays a set-A feature).

**Explicitly excluded** (the game owns these): transition durations,
engine-owned animation curves, automatic easing, timers, any required
animation or state-machine framework. The engine only applies the
supplied blend amount.

## 6. The renderer seam (the implementation notes)

- `WorldRenderer::BuildGiVolumeSetFromPaths(lines, ...)` — the
  transactional build core over an explicit `.givolume` path list
  (`BuildGiVolumeSet`'s per-volume load/upload/pack verbatim).
- `AdoptGiVolumeSet(paths)` / `AdoptGiVolumeBlend(from, to, alpha)` /
  `SetGiBlendAlpha(alpha)` / `ClearGiBlend()` — the publish entries
  (the hot-swap pattern; the old generation retires deferred). A
  scene load resets any external set (the scene's own bindings own
  the state at the scene boundary).
- `FrameUniforms` carries the B-set tables in an APPEND-ONLY tail
  (`giVolumesB`, `giVolumeCountB`, `giBlendEnabled`, `giBlendAlpha`,
  the `*B` layer/mean/vis tables at pinned offsets; the HLSL mirror
  in `common.hlsli` matches byte-for-byte, pinned by
  `static_assert`s). `sg_gi_volume.hlsli` carries the B-set twins
  (`SgGiVolumeSampleB`, `SgGiVolumeCompositeSetB`,
  `SgGiVolumeSampleMeanB`, `SgGiVolumeRayPrepB`,
  `SgGiVolumeRayCompositeB`) — the A-set hot paths pay zero cost
  when blending is off (a uniform `giBlendEnabled` branch).

## 7. Verification

- **Compile-time:** `tests/test_asset_query.cpp` (the regex engine's
  full grammar matrix + diagnostics, over a synthetic generated
  catalog — 47 checks + the static_assert pins) and the sample's
  `static_assert`s (the query against a REAL generated catalog).
- **Emitter:** `catalog.header-query-block`, `catalog.header-symbol-
  table`, `catalog.gi-*` (the namespace, the Assets row, the
  fingerprint fold, the 0x6xxx id range) in
  `tests/test_project_pipeline.cpp`.
- **Facade:** `am.*` in `tests/test_pak_host.cpp` (the GI manifest
  parse + dedup + kind refusals + the generational handle registry +
  the null-pointer batch convention; CPU-only).
- **GPU:** the renderer blend legs verify through the sample +
  headless pins (the GI hot-swap discipline); the dedicated
  two-bake-set GPU rendering regression is the documented follow-up.
