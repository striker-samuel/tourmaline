# Content-as-Code: The Query, Loading, and GI-Blend API

**Status:** v1 implemented (the query API + the GI catalog lane + the
AssetManager facade + caller-controlled GI blending). The mesh
spawn-side surface (`World::SpawnMesh(MeshHandle)`) and the
subset/pak-grouped mesh fast path are the documented follow-ups.

**Surface:**
- `engine/include/Engine/Asset/asset_query.h` — the compile-time
  query core (the regex engine + `MatchAssetsImpl`).
- `app/src/AssetManager.h/.cpp` — the loading facade.
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
  loads each owning cluster once. `MeshBatch` carries one handle per
  input key, in input order.
- **Resource handles stay separate from world entity handles**
  (loading never spawns entities). `MeshWorldIndex(handle)` exposes
  the world mesh row for the spawn-side seam.
- v1 mesh loading loads the mesh's OWNING SCENE CLUSTER through the
  normal deserializer (the pak-format boundary: meshes ship inside
  their scene's cluster; materials + textures resolve exactly as
  scene loading). The join is the `[ids]` row's `.geometry` path =
  the pak's Mesh-blob URI prefix. A multi-scene pak whose scenes
  split one family refuses loudly (the documented v1 boundary).
- `keepResident` (default) holds the cluster for the session; the
  subset/pak-grouped fast path and the shared-cluster refcount are
  the documented follow-ups.

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
