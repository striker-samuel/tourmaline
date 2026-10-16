# Artifact DB funnel: import → artifacts → DB → VFS → pak

Durable record for the pipeline rework (naming, staging, schema).
Status: spec agreed, implementing in slices (see §8).

## 1. The chain (owner-directed)

```
importer ──▶ artifacts ──▶ REGISTERED artifacts ──▶ scenes ──▶ scene byproducts
                                                                      │
                                                               ┌──────▼──────┐
                                                               │ ARTIFACT DB │  ◀── entry point:
                                                               └──────┬──────┘      every byte downstream
                                                                      │             of import is addressed
                                                                      ▼             here (canonical VFS
                                                                     VFS            path + hash)
                                                                      │ (DB-backed mount over loose)
                                                                      ▼
                                                                     pak (pure serialize + embed + verify)
```

- **Input assets stay as-is** with their `.meta` sidecars (GUID identity). Nothing new is written beside sources.
- **Derived artifacts** carry manifest UUIDs (`.gltf.meta`, stable across reimports) and register `(klass, uuid)` rows; blobs live in the hidden folder schema (§4).
- **Loose files keep exactly two jobs**: authoring layer + runtime dev overrides. The *pipeline* never depends on loose paths; it resolves through the DB.

## 2. Naming: import / cook / pack

The exe/lib names already match — the rework is enforcement:

- **import** = raw → artifacts beside sources, deterministic, nothing else
  (`import.exe`, MeshImporter). Truth fix only:
  `tools/import/main.cpp:32-36` still claims pak does cubemap/LOD cook.
- **cook** = artifact→artifact transforms + transport bakes, writes back
  beside sources + registers byproducts. `cook.exe`/`CookLib` stay;
  **GiBaker is "the GI cook"** in docs (modes = cook stages with own CLI:
  `--out` world, `--cv-spec`, `--regular-out`). `cubemap_baker`:
  deprecate to `cook --cubemap` (still built at root
  `CMakeLists.txt:338`; verify the `engine_resources` mount claim first).
- **pack** = serialize + embed + verify, pure function of its inputs.
  Concretely: move `EnsureWorldSun` + env-sun resolution out of pak
  (into cook write-back / a `cook --seal` step). After that, `pak --tour`
  keeps the flag only as read-only replay for serialization.

## 3. Staging: the DB, not a folder

No staging folder. The artifact DB **is** the staging area:

- cook/GiBaker register every output: `(klass, uuid)` → uri, hash,
  payload location (class pak) or loose-backed (identity-only rows).
- A DB-backed `IVfsBackend` mounts the funnel over the loose authoring
  layer (`db > project > content > pak`); pack walks the VFS
  (`ListDir` union exists) instead of stem-guessing.
- Precedents already in tree: `MemBackend` ("cooker staging"),
  `ReadPakImage` + `PakBackend::Open` mounts (editor_app.cpp), overlay
  priorities + `FindProviders` diagnostics, `--no-vfs` proving pak
  self-containment from the other end.

## 4. Schema (agreed)

- **Canon kinds** (`artifact_kind.h`, frozen, fail-closed validation):
  `texture | mesh | material | animation | light | camera | scene | gi`.
  `gi` covers ALL GI byproducts (sggi/cvol/regular/cubemap/lightmap),
  keyed by canonical VFS uri. No `level` klass (pack-time assembly).
  `gi` rides Texture TOC blobs (VFS-path serving rule); `animation`
  rides Texture until a dedicated `PakAssetKind` + consumer exist.
- **Folders** (hidden on Windows via root attribute, inherited —
  already true, keep; dotted root for POSIX):
  `<project>/.tourmaline/{cache.db, assets.db, pak/<klass>.pak}`
  (file names singular by construction: `pak/<klass>.pak`). Old
  `<project>/artifacts/` trees ignored in place (both DBs are
  documented-regenerable: delete + rescan/recook); rescan-skip and
  asset-ops-refuse cover both names during transition.
- **Rows**: `(klass, uuid)` primary; `uri` secondary index (canonical
  VFS path); `hash` = output content hash for deterministic byproducts
  (identity == content; staleness falls out); `Register` never touches
  payload columns, `Put` never clobbers a registered uuid.
- **UUID source**: the `.gltf.meta` manifest (stable across reimports);
  registration happens in the derive path (editor import hook;
  CLI import has no project context — pack-time/cook-time registration
  covers CLI flows, see §6 P0).

## 5. Evidence that forced this design (all observed live)

- **Dead `giBlobs`**: cook's `Run()` collects in-process cvolumes +
  re-read sglm into a vector nothing consumes (cook writes no pak).
  Comments insist they are "embedded into the pak". Without a loose
  manifest the bake succeeds and its output vanishes. Sponza only
  worked because GiBaker's manifest tripped the skip branch.
  Verdict: wire it (loose write + register), don't delete — the
  no-sidecar fallback is intended by the editor flow.
- **Bistro stub manifest**: `bistro.gltf.meta` carried zero material
  entries → zero bound geometry. Regenerating the 254-entry manifest
  restored full binding (254 materials, 129 textures, 4.2M tris).
  A funnel inspector ( §7.1) would have caught this on sight.
- **No-tour scale explosion**: `BuildFromMeshAsset` never marked nodes
  changed → identity globals → cm-as-meters → 26M-leaf octree → LZ4
  failure after 43 min. Fixed (mark all mapped nodes); tour path was
  immune via its own edits.
- **StaticSun**: bake harvest takes static lights only; both
  `EnsureWorldSun` copies now flag the sun static (runtime exempts
  static directionals; lightmap double-count deprecated + off).
  Verified behavior-preserving (recook means match <1%) with a
  rooftop diagnostic (mean 2600 — sun NEE fires).

## 6. Migration order (each shippable alone, tree green throughout)

- **Done**: spec commit one (kinds, layout, uuid schema, derive-path
  registration, 122/122 CacheTests + 32/32 ctest).
- **P0**: byproduct registration (cook loose outputs + GiBaker, both
  joining the project cache by content-root convention — no flags)
  + DB-backed VFS backend. Zero behavior change.
- **P1**: pack-from-VFS (ListDir discovery; stage manifest falls out as
  the pack receipt) + WorldSun/env move to cook.
- **P2**: registration into remaining derive paths; CLI coverage.
- **P3** (only if P0–P2 prove the model): GUID-native scene composition.

## 7. Visualizations

- **Funnel inspector** (editor, LIGHTING-gated overlay idiom): DB rows
  vs loose tree — green/registered, amber/hash-mismatch, red/unregistered
  bake. Catches stub manifests and dead vectors on sight.
- **Bake-input overlay**: which meshes feed which bake.
- **Cook timeline strip**: per-stage timings in the GI panel.

## 8. Open questions (owner calls)

1. Byproduct identity: content-hash uuid (current: identity == content
   for deterministic bakes) vs minted GUIDs persisted somewhere?
2. Scenes: is a hashed immutable scene snapshot needed so byproduct
   hashes mean "built from scene S with settings G", or is
   (scene-stem + settings-echo) sufficient?
3. GC policy for superseded rows/loose orphans (old `bistro_32..100`
   KTX2s, superseded `.sggi`): unpinned-superseded auto-GC with
   dry-run, or manual?
