# Asset identity and artifact pipeline: architectural review

> Prepared by **ChatGPT (OpenAI GPT-6)** on 2026-11-12. This is a source-inspection review, not a build-verified implementation.

## Summary

The Bistro GI build exposed inconsistent meanings of `content-dir`: the editor records `content/models/bistro/bistro.geometry` relative to the project, while GiBaker can interpret references under a physical `content/` root, causing a duplicated prefix. Fixing the spelling is not a sufficient architectural solution.

Tourmaline already has strong pieces: project GUIDs, `.meta` source GUIDs, `SourceAssetDB`, artifact cache, build plans, typed catalog keys, and a convergent runtime `ContentRegistry`. The missing contract is the translation between their identity domains.

## Findings

| Concern | Code evidence | Risk / failure | Direction |
| --- | --- | --- | --- |
| Ambiguous content root | `tools/project_pipeline/src/project.cpp`: `Project::ContentRoot()` returns `root`; `Project::Init` creates `content/models` | Logical `content/models/...` vs `models/...` depends on caller's mount | Separate ProjectRoot, ContentRoot, logical AssetPath and VfsPath |
| Duplicate project parsing | `editor/src/editor_project.cpp` and `tools/project_pipeline/src/project.cpp` independently parse marker rows | Drift between CLI/editor conventions | One ProjectContext parser and root resolver |
| Substring-based root derivation | `editor/src/editor_cook.cpp` scans for `content/` inside a host path | Wrong roots, CWD sensitivity, false substring matches | Resolve against opened project and registered asset record, never string slicing |
| Multiple tour resolution lanes | `tools/gltf_import/src/tour_world.cpp` applies source references separately from texture/material pools; pool paths fall back to as-is | Silent CWD fallbacks and differing editor/CLI behavior | One typed resolution API with deterministic errors |
| Source vs runtime identity split | `tools/artifact_cache/include/artifact_cache/assetdb.h` describes GUID source identity but path-based cooked/TOC identity | Renames may lose downstream association | Carry source AssetId through graph; map to runtime IDs at pack time |
| Path-keyed import registry | `tools/project_pipeline/src/registry.cpp` updates records by source path despite storing GUID | Rename produces stale or duplicated registry entries | GUID-primary lookup, path as mutable metadata |
| Generated data beside authored sources | `tools/project_pipeline/include/project_pipeline/project.h` documents derive-beside due to relative material references | Harder cleanup/rebuild, generated vs authored data mixed | ArtifactStore-owned products and explicit dependency references |
| GI manifests and naming | `tools/gi_baker/main.cpp`, `editor/src/editor_cook.cpp`, `engine/include/Engine/Asset/tour.h` describe manifest emission and scene/bake bindings | Missing/stale manifest may be confused with intentionally optional GI | GI artifact dependency keyed by scene/volume GUID + recipe, atomic publish |
| Packaging/linkage | Build recipe/pack stages must recover dependencies from filenames and references | Incomplete output or difficult diagnostics | Package verified transitive artifact closure |
| Runtime migration risk | `AGENTS.md` and `docs/content_as_code.md` document convergent ContentRegistry and typed keys | Full runtime rewrite would threaten catalog ABI | Preserve runtime loading and pak format during build-side migration |

### Additional tests needed (not claimed as confirmed bugs)

1. Project moved to a different drive, scene opened from a different working directory.
2. GUID-stable asset moved or renamed; source and derived references remain intact.
3. Two scenes with identical stems but different directories.
4. Bake canceled after writing some outputs; old generation must remain valid.
5. Bake input or recipe modified without changing scene stem.
6. Missing required GI compared with explicitly disabled/optional GI.
7. Source identity resolved identically by editor, CLI and CI.
8. Duplicate or ambiguous VFS mount keys must error, not silently choose.
9. Build rerun uses fresh cached products without incorrectly reusing stale ones.
10. Pack manifest and payloads are published atomically; no partial GI state.

## Proposed architecture

Define *distinct types*, rather than passing untyped strings:

- `ProjectId`: persistent project GUID, already supported.
- `AssetId`: durable GUID in authored source `.meta`, stable through move/rename.
- `AssetPath`: canonical user-facing path relative to physical `<project>/content/`, e.g. `models/bistro/bistro.gltf`; not a durable ID.
- `HostPath`: absolute filesystem path, used only at an I/O boundary.
- `VfsPath`: explicit mounted virtual path, distinct from AssetPath.
- `ArtifactKey`: hash of source/dependency hashes, normalized recipe, tool version, platform configuration; versioned derived product.
- `RuntimeAssetId`: existing compact typed catalog ID.

A `ProjectContext` owns project root, physical content root, mounts and asset registry; it alone converts asset selectors/IDs into concrete paths. An `ArtifactGraph` maps source assets to deterministic derived products and dependency edges. Import/Bake/Cook receive resolved graph inputs; Pak verifies a dependency closure. The existing runtime ContentRegistry and catalog ABI remain in place.

Recommended layout:

```
<project>/project.tourproj
<project>/content/          # authored sources and .meta GUIDs
<project>/.tourmaline/
  assets.db                 # regenerable GUID -> path index
  artifacts/                # derived products by ArtifactKey
  builds/                   # dependency graph, staging
<project>/paks/             # only deployment products
```

A normal build should accept one project and one human-friendly scene selector (e.g. `Project.exe build --project <root> --scene scenes/bistro.tour`), resolve it *once*, then pass typed values to the stages. Legacy `content/...` references must be converted at explicit compatibility boundaries, not stripped opportunistically based on existence.

## Incremental implementation sequence

1. **Path invariant and tests:** canonical project/content roots; typed validated logical references; reject absolute, escaping and duplicated-prefix input. Maintain an explicit legacy conversion path.
2. **Unified editor/CLI project context:** delete duplicate marker parsing and substring-based root inference. Route GiBaker/Cook/Pak reference binding through one resolver.
3. **GUID-primary asset registry:** source path is mutable metadata; migrate existing path-keyed rows.
4. **First-class artifact DAG:** recipes + tool versions + dependency digests produce versioned immutable outputs.
5. **GI bake as graph product:** scene GUID, volume identity, settings and outputs explicitly bound; transactional manifest/payload publication and required/optional GI policy.
6. **Dependency-driven packaging:** verify all required products, preserve existing pak format and generated C++ typed bindings.

## Acceptance criteria

- `content/models/...` cannot be interpreted differently by two stages; malformed new refs fail clearly.
- Moving project or GUID-stable source does not invalidate identity or create duplicate assets.
- Editor and CLI resolve identical input sets and artifact keys.
- Missing mandatory GI fails a build before pak publication; disabled GI is an explicit supported case.
- Stale or partially published products are not accepted as current.
- No regular user workflow requires guessing manifest filename stems or editing serialized paths manually.
- Existing pak/catalog ABI and runtime provider overrides remain compatible.

**Disclosure:** Written by ChatGPT (OpenAI GPT-6). Findings are based on the cited source files and the reported Bistro incident; no native compilation or runtime bake was performed in this review PR.
