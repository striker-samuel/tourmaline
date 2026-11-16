# Caldera source audit — findings and schema dispositions

Status: **Phase 1 (source audit) initial pass.** The content model below was
produced by `tools/caldera_audit.py` against the public Activision Caldera
dataset (https://github.com/Activision/caldera), cloned to `G:\caldera_test\caldera`.

Dataset scale (measured):

| Metric | Value |
|---|---|
| USD files | 63,684 (63,683 binary `.usd` + 1 `.usda`) |
| Working-tree bytes | 6.79 GB |
| Up axis / units | Z-up, inches (`metersPerUnit = 0.0254`) |
| Map footprint | ~2 × 2 miles (~3.2 × 3.2 km → ~13 × 13 × 256 m root cells) |

## Tooling

`tools/caldera_audit.py` (Python, `usd-core`) provides two modes:

- `inventory <root>` — file-level, uncomposed. Walks every USD layer and
  counts authored prim specs **across all variant opinions** (so both the
  `districtLod` proxy and full alternatives are represented). Safe over the
  whole 63 k-file tree (no full composition).
- `compose <stage> [--variant name=value] [--load-payloads] [--cell-size N]`
  — fully composes one stage (a district `.usd`, `caldera.usda`, or any
  sub-scene), resolves references/payloads/variants, and reports prototype
  counts, instance counts, estimated vertex/index bytes, unsupported schemas,
  and a per-256 m-cell working set.

`--variant districtLod=full` selects the full representation of the aggregated
`caldera.usda` districts (the authored default is `proxy`). Standalone district
and prefab files under `map_source/` are already full detail.

## Authored schema inventory (all 63,684 files, all variants)

| Disposition | Prim specs | Notes |
|---|---|---|
| structural | 1,104,356 | Xform 981 k, Scope 47 k, untyped 76 k |
| resource | 902,564 | GeomSubset (material subsets) — every Mesh ~1 subset |
| mesh | 890,840 | renderable `Mesh` prims (includes proxy + full variants) |
| primitive_shape | 214,562 | Cube 210 k, Sphere 4 k — collision/lighting volumes |
| unsupported | 6,813 | SkelRoot 3,874 + Skeleton 2,939 |
| must_implement | 14 | PointInstancer (terrain clutter) |
| camera | 13 | authored cameras |
| points | 7 | player data (`/players/endpoints`, `/players/breadcrumbs`) |

## Schema disposition (the "no silent discard" contract)

| Schema | Count | Disposition |
|---|---|---|
| `Mesh` | 890,840 | **Import** — renderable geometry (the core target). |
| `GeomSubset` (materialBind) | 902,564 | **Consume as material split**, not geometry (importer already does this). |
| `Xform` / `Scope` / untyped | 1,104,356 | **Structural** — preserve hierarchy. |
| `Cube`, `Sphere` | 214,562 | **Exclude by default** — collision/lighting volumes ("alternative, usually invisible representations"). Emit an auditable omission count. |
| `PointInstancer` | 14 | **Implement** — the terrain clutter path (highest-risk importer work). ~285 k instances per terrain section × ~14 sections ≈ several million instances. |
| `SkelRoot` + `Skeleton` | 6,813 | **Disposition TBD** — animated rigs, all under `assets/` (not placed map geometry). Likely import-as-static (bind pose) or exclude for the geometry-only viewer. |
| `Camera` | 13 | **Import** — authored viewpoints (useful for test scenes). |
| `Points` | 7 | **Exclude** — player analytics, not map geometry. |

No other authored schema types were found; the importer's remaining
fail-closed cases (subdivision surfaces, instance proxies, connected shader
graphs, `resetXformStack`) are not present in the Caldera source, so they do
not block this dataset.

## Per-district content (Mesh prims, authored, all variants)

The map is organised as ~48 granular directories under
`map_source/prefabs/br/wz_vg/mp_wz_island/`. Mesh prims concentrate in shared
building libraries (referenced by many district assemblies) rather than in the
district files themselves:

```
urban            78,863      chem_factory      56,529
industrial       78,680      residential       53,188
season_3         70,926      map_docks         50,769
commercial       69,789      assets            49,559  (+6,813 skinned)
map_arsenal      64,236      military          37,919
season_4         63,447      ...                ...
poi              58,387      map_capital       22,856
```

- PointInstancers: **all 14 in `superterrrain/`** (the `st_*` terrain sections).
- Skinned rigs: **all in `assets/`**.
- `map_source/` root and `season_4` hold the bulk of the `Cube` collision volumes
  (82 k and 58 k respectively); `layers/` holds the 4,187 spheres (lighting).

## Working-set calibration (compose mode, full detail)

Per-stage source vertex/index bytes (positions + normals + UV + indices), the
faithful relative measure — NOT the cooked meshlet/quantised size. The
compose mode deduplicates by **content identity** (the strongest defining USD
source asset), so `unique mesh bytes` reflects what the importer must keep
resident after dedup, not the naive composed-mesh sum.

| Stage | Placements | Unique assets | Dedup | Unique bytes |
|---|---|---|---|---|
| `season_4/map_capital.usd` | 381,970 | 174,181 | 2.19× | 50.18 MB |
| `season_4/map_airfield.usd` | 293,360 | 110,181 | 2.66× | 35.58 MB |
| `season_4/map_beachhead.usd` | 137,066 | 39,552 | 3.47× | 15.74 MB |
| `season_4/map_phosphate_mine.usd` | 113,793 | 41,052 | 2.77× | 18.44 MB |
| `commercial/hotel_01.usd` | 27,418 | — | — | ~12 MB |
| `superterrrain/season_4/st_e.terrain.usd` | 1 (terrain) | — | — | ~19 MB + 285 k clutter instances |

## Critical importer requirement: content-identity dedup

Caldera places buildings by **USD reference**, not instance proxies: the same
source asset is composed into many distinct prim paths. Measured on Capital,
433 k composed mesh prims collapse to 182 k unique source assets (2.38×); a
single gutter-support mesh is referenced 712×, loot crates 454×, shared
building parts 416×.

The current `usd_import.cpp` emits every composed `Mesh` prim as a unique
tinygltf mesh, so it would produce ~434 k meshes (117 MB) for Capital alone,
of which ~182 k (50 MB) is unique. **The importer must dedup by content
identity and emit shared meshes once with many placement nodes** — this is the
dominant dedup opportunity, larger than the 14 PointInstancers (which are a
separate, smaller mechanism for terrain clutter). The same tinygltf reuse
mechanism (`node.mesh` index shared across nodes) serves both.

The terrain/clutter split is the key memory insight: each terrain section is
dominated by its heightfield mesh (~19 MB), while hundreds of thousands of
clutter instances share a few dozen small foliage prototypes (~75 KB total) —
instancing keeps the foliage working set tiny.

## Open follow-ups before importer/streamer changes

1. Compose every district at `districtLod=full` (per-district, memory-bounded)
   to produce a total full-geometry working set and per-cell histogram.
2. Decide the `SkelRoot`/`Skeleton` disposition (static import vs omit).
3. Confirm the `Cube`/`Sphere` exclusion list against a district with known
   collision volumes, so the omission is explicit rather than heuristic.
4. Verify the PointInstancer prototype mesh-byte attribution across all 14
   terrain sections (the 46-prototype `st_e` case is the reference).
