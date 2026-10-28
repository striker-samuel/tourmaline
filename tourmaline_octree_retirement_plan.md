# Tourmaline: retire the legacy SG-GI octree

Target: world tiles + authored convex `.givolume` GI, with regular GI volumes retained as their own fallback. Legacy `.sggi` is removed in stages to avoid deleting an asset that build/cook still requires.

## Stage 1 — stop using the octree (accompanying patch)

Files: `engine/include/Engine/Renderer/WorldRenderer.h`, `engine/src/Renderer/WorldRenderer.cpp`.

- Do **not** invoke `LoadGiOctree()` at scene load. Explicitly reset octree CPU/GPU state on scene rebind so `frame.giEnabled` stays off.
- Change `GetActiveGIVariant()` so convex GI volumes select `GIVariant::Full` **without** `m_giLoaded`. The existing `GI_FULL` shader handles the no-octree case via sky fallback when `frame.giEnabled == 0`.
- Register only WorldFogDensity (field 0); do not register world-field mean/coverage providers that generate bricks by sampling the removed CPU octree. The volume mean textures are independent; `SampleFogGi` handles the missing world-field fallback.
- Make World Map (C) draw the loaded convex GI volume XZ lattices (one rendered dot per 6-pixel screen cell), and report total 3D lattice sites and drawn XZ dots. Do not use `m_giOctree.probes`.
- Replace the legacy octree statistics with the actual convex and regular GI-volume counts.

**Preserved:** `.givolume` load path, regular volume path, environment world tiles, reflection cubemaps, fog density world field.

**Intentionally not removed yet:** legacy loader implementation; `.sggi` file parsing and baking; pak binding enums; GI_OCTREE shader permutation; CPU fog-GI source classes; old octree UI/debug diagnostics elsewhere. These still compile but are not active during scene load.

## Stage 2 — stop generating and requiring `.sggi`

1. Search the build pipeline and project templates for `.sggi`, `--out`, `giAsset`, `GiBinding::Kind::Probe`, `m_giProbeName`, `LoadGiOctree`, `SgGiVolume`, and `gi_baker`. Identify **which producers** create `.sggi` and **which consumers** insist on it, especially `Project.exe build`, `GiBaker`, `PakCooker`, `ResolveGiBakes`, project manifest validation, and stale-bake/freshness checks.
2. Make an authored `GiVolume` binding sufficient for a freshly cooked pak. Do not demand a `Probe` binding or a `.sggi` artifact. For worlds without authored GI volumes, retain normal sky and/or regular-volume fallback; do not silently require the old octree.
3. Stop scheduling octree bakes. Remove octree-specific generator outputs from bake manifests, freshness metadata and the VFS asset registry. Keep *per-volume* `.givolume` bakes and cubemap/reflection outputs.
4. Bake a clean scene to a **new** output folder. Confirm that build/cook succeeds with no `.sggi` present. This is the gate before code deletion.

## Stage 3 — delete dead octree implementation

- Remove the now-unused `WorldRenderer::LoadGiOctree()` definition/declaration, `m_giOctree`, `m_giLoaded`, `m_giGridBuffer`, `m_giGridAddress`, `m_giLevelTextures`, and octree-only frame bindings once all references have been migrated.
- Remove the octree-only CPU fog-GI brick source classes, globals and fields. Keep fog density field and the GI-volume fog mean layers.
- Delete the `GIVariant::Octree` selection and `GI_OCTREE` permutation after auditing all lit/mesh/filtered shader variants and specialization IDs. Only then rename `Full` to `Volumes`, updating all C++/HLSL and tests together. Do not blindly renumber shared ABI layouts.
- Delete the octree-specific `SgGiVolume` serializer, dense-level GPU packer, old baker entry points, and shader walkers **only after** confirming they are not reused by regular volumes or SG helper math. Preserve shared `SG` basis/lighting utilities.
- Clean up the remaining inspector text, comments, docs, tests and examples; remove misleading wording that local GI volumes need an octree underneath them.

## Validation checklist

- `git apply --check tourmaline_retire_octree_runtime.patch` succeeds against the matched repository revision, then apply it.
- Build the `App` and editor targets; build shader permutations including `GI_FULL` with no octree data.
- A scene with **only** `.givolume` lighting still shows baked GI on static surfaces (no `.sggi`).
- A scene with **no** GI volumes displays the proper sky or regular-volume fallback rather than stale octree data.
- Switching scenes cannot resurrect an octree pointer/buffer; `frame.giEnabled == 0` on both frames.
- World Map (C) shows volume lattices and honest counts. Note: these are lattice sites, not proof every sample is valid or inside the convex hull.
- `TOURMALINE_GI_VOLUMES=0` deliberately drops convex volume GI to regular/sky fallback; no invisible octree fallback.
- With `TOURMALINE_WORLD_FIELDS=1`, fog density streams, world-field GI mean/coverage are absent, and volume-based fog shading/sky fallback behave as expected.
- Rebuild a clean pak without `.sggi` and verify the cooker, pak registry, and runtime all accept it.
- Capture A/B screenshots of interiors, transition/fade edges, open terrain and volumetrics before merging stage 3.

## Apply stage 1

From the repository root on an **unmodified matching checkout**:

```sh
git switch -c retire-legacy-sggi
git apply --check tourmaline_retire_octree_runtime.patch
git apply tourmaline_retire_octree_runtime.patch
git diff --check
```

If you have applied an earlier World Map probes patch, do not layer this patch on top unchanged; revert that patch or rebase the overlapping map hunks first. This patch supersedes the earlier map-only fix.

**Status:** Patch is syntax/format-checked using a synthetic source fixture rebuilt from the public GitHub snippets. No live compilation, runtime lighting validation, or `git apply --check` against a full repository checkout was possible in this environment. Run the checks above on your checkout before merging.

## Execution log (the retire-legacy-sggi branch)

- **Stage 1 (8e6cf74e):** runtime retirement landed — no `LoadGiOctree()`
  call, explicit rebind reset, `GIVariant::Full` without `m_giLoaded`,
  only WorldFogDensity (field 0) registered, the World Map lattice
  layer + honest counts, the legacy stats replaced. Validated on
  sponza_3 (octree-off log lines, volumes load + composite, interior
  capture stays lit, `TOURMALINE_GI_VOLUMES=0` drops visibly, the
  fields run registers only the fog field).
- **Stage 2 (352c58a8):** the pipeline gate — the project pipeline's
  GiBaker stage passes only `--content-dir` + `--tour` (volume-only);
  the pak's world-field freshness gate retired (the record is
  acknowledged + ignored, `.givolume`/cubemaps keep their gates). Clean
  bake to a NEW folder: zero `.sggi`, the pak builds + loads + stays
  lit. THE DELETION GATE PASSED.
- **Stage 3 (runtime half):** `LoadGiOctree` defn/decl, `m_giOctree`,
  `m_giLoaded`, `m_giGridBuffer`, `m_giGridAddress`, `m_giLevelTextures`,
  `m_giLevelLayers`, `GiActive()`, `GiProbeVisibleFromCamera`, the probe
  overlay + its UI + the `TOURMALINE_GI_POINTS`/`GI_OCCLUDE`/
  `GI_DEBUG_LEVEL` pins, the octree debug views 3/4, the CPU fog-GI
  brick sources + `g_fogGiVolume`, `m_giProbeName`, the GI_OCTREE
  permutation (CMake rows + loader skip), the GI_FULL→GI_VOLUMES
  rename (ID 3 preserved, ID 1 reserved — NO ABI renumber), the shader
  walkers (`SgGiSample`/`SgGiSampleMean` + probe helpers) — all deleted;
  the shared SG basis/irradiance/specular math preserved in
  `sggi.hlsli`; the frame `gi*` block stays the zeroed legacy ABI.
  Render-identical: interior 11,869/15,516/max 15 and volumetrics
  (matched pins) 7,098/8,231/max 10 — both BELOW the measured
  run-to-run noise floor (~11.5k/15,046/max 20 for identical-source
  runs). 42/42 ctest.

- **Stage 3 (data-layer half — DONE):** the audit confirmed the
  deletion is SAFE: the convex-volume bake (`BakeGiVolume`,
  cubemap_volume.cpp) is fully self-contained (its own direct per-probe
  trace, no bake.cpp octree internals), so `BakeSgGiVolume` +
  bake.cpp's ~12 anonymous-namespace octree helpers + `SgGiVolume`
  (probe_volume.h/.cpp: serializer, dense packer, Sample) + the
  GiBaker `--out` block + test_gi's `.sggi` round-trip section are all
  octree-only — and all DELETED. The retirement reached the tool/data
  edges the runtime audit surfaced:
  - bake.h/.cpp keep ONLY the shared trace substrate
    (GiTraceScene + the KTX texel decode + BuildTraceSceneFromCluster)
    — `SgGiBakeConfig`/`SgGiBakeStats`/`BakeSgGiVolume` and the
    brick/relocation/refinement namespace are gone (bake.cpp
    1779 -> 952 lines); `WorldBakeUnitHash`/`CanonicalWorldCfgHash`
    deleted from bake_inputs (GiVolume/Cubemap/Region hashes stay).
  - `probe_volume.h/.cpp` deleted outright; every include site
    migrated (WorldRenderer.h, gi_baker, test_gi, editor_app.h; the
    lightmap.cpp half-pack include that rode the deleted header now
    includes sg.h directly).
  - GiBaker: the `.sggi` analyzer (AnalyzeVolume) is retired
    (`--analyze` wants .sgrv/.givolume); `--out` PARSES but Run
    REFUSES a non-empty out loudly (exit 2) so old scripts + stale
    in-process callers cannot silently bake volumes + exit 0 looking
    successful; the oct-only Args knobs deleted (errorRays/root/
    minBrick/maxProbes/tolerance/nearFactor/waves/diffusion/
    smoothingPasses/support — rays/bounces/threads/verify stay for
    the regular/convex bakes); the minted-default `out` derivation
    gone; the world-bake tail (freshness check, bake, dense bake,
    write, world binding + hash stamp, verify) replaced by the honest
    "no bake stage requested" exit 0 — the pipeline schedules GiBaker
    unconditionally, so a volume-less project must stay green (a bare
    `--scene` run that used to world-bake a minted .sggi now reports
    nothing-to-bake; verified: exit 2/exit 2/exit 0).
  - The EDITOR's world legs retired: both in-process `gibaker::Run`
    call sites (cook stage 1 + Bake GI) are convex-volume-only (no
    minting, no `gargs.out`, no maxProbes budget note); the `.sggi`
    density overlay (RefreshSggiDensity + the lattice dots + the
    legend + the reload/toggle UI) died with the loader — the
    cubemap-density half stays; the `ExportCfg().giWorld` dialog
    checkbox + the TourCook field retired (the tour token stays
    ACCEPTED — `setExportAttr` hard-fails unknown attrs and every
    pre-retirement tour carries the line — but ApplyExportSettings
    acknowledges + ignores it); `GiCookAsset::world` deleted (old
    sidecars' world= lines load through the unknown-key tolerance;
    the hash drops the line by design), the legacy world-record
    blobPath snapshot kept while a tour still carries a World record.
  - The pipeline's `giBackend`/`giMaxProbes` settings pair removed
    (the stage-2 comment promised stage 3 does it); cook's
    `--gi-probe` help marked legacy (the binding kind stays for old
    paks; the runtime acknowledges + ignores it).
  - VALIDATION: full build (App + Editor + every tool) + 42/42 ctest
    (test_gi keeps the SG/BVH/lightmap/volume tests; test_bake_assets'
    GiCook round trip updated + a legacy-world=-line tolerance check);
    the CLI contract triple verified (--out refusal exit 2, .sggi
    analyze refusal exit 2, bare --scene nothing-to-bake exit 0 with
    the trace scene still building: 262,267 tris); the stage-2
    volume-only pak loads IDENTICALLY (the same [GIV] payload stats
    voxels=1512/13608 mean=6776.02 max=25204, center 255,250,239)
    and the A/A noise floor holds (11,939/15,562/max 21 vs the
    established ~11.5k/15,046/max 20).
  - The plan's four stages are COMPLETE. The octree is fully retired:
    no runtime loader, no shader walker, no bake machinery, no
    serializer, no analyzer, no authoring flag. What remains of the
    legacy is pure ABI/data tolerance: the zeroed frame `gi*` block,
    the reserved specialization ID 1, the accepted-but-ignored
    `giWorld` tour token + Probe binding kind + World giAsset record
    fields, and old `.sggi` files on disk that nothing reads.
