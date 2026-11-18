# GFX Retirement — migration record

> Working document for the legacy GeometryFX ("GfxFilter") retirement.
> This file records the baseline, the consumer inventory that drives the
> epoch order, and the design decisions locked in before each phase.
> Update it at every epoch boundary (the epoch's commit records the delta
> here). Rule: one epoch, one independently testable result; no permanent
> parallel representations.

The external plan is nine epochs. Target architecture:

- **Static geometry** — GPU-scene registration, culling, LOD, and
  indirect rasterization (the `gpu_scene_*` subsystem, already landed).
- **Skinned geometry** — stays on direct/legacy rendering. GPU skinning
  and deformed-bounds generation are preserved.
- **Visibility** — one authoritative prepass Hi-Z pyramid per view,
  shared by static GPU-scene and skinned culling
  (`tour::visibility`, already extracted).
- **Scheduling** — GPU skinning overlaps static depth prepass and Hi-Z
  generation where safe.
- **Memory** — large upfront allocations derived from world
  requirements, not camera-dependent candidate counts.
- **Removed** — lightmaps, cone debug visualization, obsolete GfxFilter
  infrastructure.
- **Preserved** — normal-cone culling data, texture streaming,
  animation, shadows, motion vectors, required direct-only rendering.

## Baseline

- Branch: `gfx-retirement` (from `master`)
- Base SHA: `f00b4b88` (master, "pak: world manifest as a fixed-size,
  offset-indexed table")
- Working clone: `G:\SCRATCH\tourmaline_refactor` (moved off the
  low-space `F:` tree for this refactor)
- Windows toolchain: VS 18 Insiders MSVC 14.52 (vcvarsall x64), Ninja,
  `EMBREE_PREBUILT_DIR=F:/tools/embree-4.4.1`, `KTX_ROOT=C:/ktx`.
  Linux verification per AGENTS.md (Ubuntu 24.04 + GCC 14 in WSL).
- Baseline test run (`out/build/x64-Release`, the GFX-relevant subset):
  `architecture_tests`, `rendergraph_contract_tests`,
  `lightmap_pull_tests`, `gpu_scene_world_tests`, `gpu_scene_lod_tests`,
  `gpu_scene_candidates_tests`, `gpu_scene_validation_tests` — **7/7
  passing** after one pre-existing baseline fix: the world-manifest
  commit (`f00b4b88`) added five `smvk::` compat re-exports to
  `Engine/Asset/pak.h` without bumping the R7 alias-containment row
  (29 → 34), so `architecture_tests` failed at baseline; the row was
  corrected in the `fix(test):` commit preceding this one.

## Inventory — the two architectures at baseline

The filter render path is already retired: `gfxfilter.cs.hlsl` /
`gfxfiltered.vert.hlsl` / `gfxbatchcull.cs.hlsl` are deleted,
`GfxRenderer::AppendPasses` is a no-op, and the GPU-scene raster shaders
(`gpu_scene_raster_*.hlsl`) are the replacements. What remains is the
legacy *ownership shell*: `GfxRenderer` still owns the static geometry
registration, the `gfx::State` data model, the filtered VS + its lit
program twins, and the Hi-Z wiring, all consumed through WorldRenderer.

### Legacy GeometryFX surface (retire)

| File | Lines | Role |
|---|---|---|
| `engine/include/Engine/Renderer/GfxRenderer.h` | 616 | `engine::GfxRenderer` (IFrameRenderer): owns one `gfx::State`, the filtered VS + every lit program twin, mesh registration, Hi-Z passes, lightmap chart upload |
| `engine/src/Renderer/GfxRenderer.cpp` | 848 | impl: `Ensure` (GPU-scene init path), `AppendHizPasses`, `PrepareGpuSceneHiz`, `RegisterWorldMeshes`, `UploadStaticData`, `EnsureChartXforms` (lightmap), `DumpCullBaseline` (debug) |
| `engine/include/Engine/Renderer/gfx_types.h` | 1133 | `tour::gfx` (alias `smvk::gfx`): `State`, `View`, `Draw`, `Batch`, `TemporalView`, `DrawRequest`, `CullFlags`, the procedural API, the lightmap fields (`LmChartXform`, `Draw::lmChunk/lmChartOffset/lmDelta`, `UvFlatVsPushTail`, `DirectLightmapPullTail`, `MaterialRange::lobeCount`), the cone-debug state |
| `engine/src/Renderer/gfx_types.cpp` | 1500 | `gfx::State`/`View`/Hi-Z/skin-bounds/cone-debug/demand impl |
| `engine/include/Engine/Renderer/gfx_dynamic_capacity.h` | 166 | `tour::gfx::DynamicCapacity`, `PlanViewCapacity`, `kDeclaredMaxDraws`/`kDeclaredMaxCandidates`, byte/dispatch planning |

Legacy shaders:

| Shader | Destination |
|---|---|
| `gfxcone_debug.cs.hlsl` | **delete** (Epoch 06) |
| `gfxfilter.hlsli` | **delete** (Epoch 08) |
| `gfxmesh.hlsli`, `gfxmesh_lit.frag.hlsl`, `gfxmesh_debug.frag.hlsl`, `gfxmesh_lit_decl.hlsli` | retire with the mesh leg |
| `gfxseed.hlsli`, `gfxseed.vert.hlsl`, `gfxseedmasked.vert.hlsl`, `gfxshadowseed.vert.hlsl`, `gfxshadowseedmasked.vert.hlsl` | retire (seed depth legs) |
| `gfxskinning.cs.hlsl`, `gfxskinbounds.cs.hlsl`, `gpu_scene_skinning.hlsli` | **preserve** (GPU skinning + deformed bounds) |

### New GPU-scene surface (retain)

- Headers: `gpu_scene.h`, `gpu_scene_raster.h`, `gpu_scene_upload.h`,
  `gpu_scene_pages.h`, `gpu_scene_skinning.h`,
  `gpu_scene_render_view_bridge.h`, `gpu_scene_instancing.h`,
  `gpu_scene_validation.h`, `gpu_scene_candidates.h`, `gpu_scene_leaf.h`,
  `gpu_scene_world.h`, `gpu_scene_hlod.h`, `gpu_scene_lod.h`,
  `gpu_scene_task_mesh.h`, `gpu_scene_meshlet_compact_abi.h`,
  `gpu_scene_meshlet_prefix.h`, `gpu_scene_meshlet_temporal.h`,
  `gpu_scene_pipeline_registry.h`, `gpu_scene_pso_prefix.h`,
  `gpu_scene_render_route.h`, `gpu_scene_raster_ownership.h`, and the
  shared-physical-geometry surface `gpu_scene_static.h` +
  `gpu_scene_raster_geometry.h` (the `PersistentGeometryRegistry`
  canonical identity authority already exists there).
- ~45 `gpu_scene*.hlsl` shaders (task/mesh, indexed raster,
  instance/meshlet/leaf cull + compaction, PSO prefix, packed skinning).

### Shared infrastructure (retain)

- `engine/include/Engine/Renderer/visibility_hiz.h` (259 lines) —
  `tour::visibility`: renderer-neutral reversed-Z Hi-Z storage, push
  contracts, `RebuildHizSlice`, `HizViewResources`. Both culling paths
  consume this format.
- `hzb.hlsli`, `hzb.cs.hlsl`, `hzb_max.cs.hlsl`, `hzb_spd.cs.hlsl` —
  the shared pyramid builders.

## Consumer classification (Epoch 01 exit criterion)

Every GFX dependency gets a destination: **retain**, **migrate**, or
**delete**.

| Consumer | Site | Destination |
|---|---|---|
| `WorldRenderer` (`m_gfxRenderer` member) | `WorldRenderer.h`, `.cpp` (~66 `State()` refs) | **migrate** — GPU-scene static + shared Hi-Z; skinned stays direct |
| `WorldRenderer::m_gfxFilterAvailable` | `WorldRenderer.h:3760`, `.cpp:449,2700,2739` | **delete** (Epoch 02/03) — GPU-scene inits independent of GfxFilter |
| `WorldRenderer::FeedGfxRendererShaderModules` + lit-twin feed loop | `.cpp:310–450` | **migrate** — filtered VS/twins move to the GPU-scene raster program owner |
| `WorldRenderer::GpuSceneIndexedTables` | `.cpp:11772,11834` | **migrate** — already reads `State()` as `IndexedGeometryTables`; switch to the static-geometry owner |
| `ssgi.h` (includes `gfx_types.h`) | `Rendergraph/ssgi.h:47` | **migrate** — needs only the Hi-Z/cull types; retarget to `visibility_hiz.h` |
| `rendergraph.h` (comment only) | `Rendergraph/rendergraph.h:70` | **retain** (comment) |
| `RenderSettingsUi.cpp` (includes `gpu_scene_static.h`) | `app/src` | **retain** — reads the static-geometry registration surface + culling-panel `config.cullFlags` |
| `editor_render.cpp` (`UvFlatVsPushTail`/`kUvFlatVsPushOffset`) | `editor/src` | **migrate/delete** — lightmap push tail (Epoch 06) |
| `editor_lmexport.cpp` (`UvFlatVsPushTail`/`kUvFlatVsPushOffset`) | `editor/src` | **migrate/delete** — lightmap push tail (Epoch 06) |
| `mesh.cpp` (comment reference) | `engine/src/Renderer` | **retain** (comment) |
| `test_architecture.cpp` R7/R8 baselines | `tests` | **update** — shrink baselines as GfxRenderer/`gfx::State` retire |
| `test_rendergraph_contract.cpp` (gfx push/stride pins) | `tests` | **migrate** — pins move to the surviving owner's header |
| `test_gpu_scene_world.cpp` (`gpu_scene_raster_geometry.h`) | `tests` | **retain** |
| `test_lightmap_pull.cpp` | `tests` | **delete** (Epoch 06) |

Note: `test_gfxfilter.cpp` does **not** exist on master — the legacy
filter's contract test was already removed. The `gfx.userdata.*` /
`gfx.family-*` test names still referenced in `AGENTS.md`,
`docs/shader_library_design.md`, `gfx_types.cpp` and
`gfx_dynamic_capacity.h` are stale; only the World-state `family.*`
pins (in `test_world_entities.cpp`) and the push/stride `contract.*`
pins (in `test_rendergraph_contract.cpp`) survive. Stale references are
cleaned up in Epoch 08.

### Static geometry fallback + rejection reasons (baseline)

Static meshes register into the GPU-scene filtered path only when
`GpuSceneCookedMeshletsWanted()` and the mesh carries either cooked
meshlets (`RegisterMeshFromCooked`) or a retained upload
(`RegisterMeshFromUpload`). The remaining static fallbacks and their
reasons:

- No cooked meshlets AND no retained upload → `UINT32_MAX`
  (direct-draw fallback).
- `GfxRenderer::Ensure` failure → `m_gfxFilterAvailable = false`
  (`.cpp:2700`) — whole GPU-scene sync collapses to direct draws.
- Zero registered clusters → `m_gfxFilterAvailable = false`
  (`.cpp:2739`).
- Non-TriangleList topology / zero or non-multiple-of-3 index count /
  out-of-capacity index or vertex range → rejected at
  `ResolveWorldSurfaceIndexedRange` (all-or-nothing per mesh).
- `RasterSphereContainsLodBounds` failure (stale/degenerate instance
  sphere) → route back through the legacy path.

## Locked design decisions

- **One prepass Hi-Z pyramid per view** (`tour::visibility`), shared by
  static GPU-scene HZB culling and skinned direct-renderer HZB culling.
  No redundant pyramid generation remains.
- **Static-only occluder prepass** is the scheduling enabler: the shared
  pyramid may be built from a deliberately static-only occluder prepass
  (conservative), which must not assume unfinished skinned depth is
  present. GPU skinning overlaps the static prepass/Hi-Z work wherever
  that dependency does not prevent it.
- **Canonical geometry identity** derives from the GPU-scene mesh's
  generational slot + LOD + submesh (`PersistentGeometryKey`), never the
  revision-local GfxFilter mesh ID. Retired keys are flight-fenced, never
  re-assigned to a different mesh.
- **Skinned geometry never migrates into GPU-scene indirect raster.**
  Direct draw submission is preserved; only its HZB culling and bounds
  pipeline are shared.
- **Capacity is world-derived, not camera-derived.** The 65,535/65,536
  ceilings and candidate-count-driven buffer growth are replaced with
  checked startup reservations (Epoch 04).
- **Namespace boundary:** new engine-owned types land in `tour::`
  (`tour::gfx`, `tour::visibility`, `tour::gpu_scene`); the `smvk::gfx`
  alias is the R7-baselined transitional spelling and shrinks as
  consumers move off it.

## Amended epoch roadmap

Commit subjects follow the plan; the deltas are marked.

- 01 — `docs(gfx): dependency audit and baseline` (this commit).
  - **Record**: full consumer inventory above; baseline captured.
  - **Gates**: inventory complete (every GFX dependency has a
    destination); `test_text_integrity.py` clean (8/8); architecture
    test (R1–R9) clean at baseline; the 7-test GFX subset green.
- 02 — `refactor(gfx): GPU-scene initialization independence` —
  GPU-scene renders static geometry even when legacy GfxFilter
  initialization is absent; remove the dependency on
  `GfxRenderer::Ensure` + `m_gfxFilterAvailable`.
  - **Record**: the GPU-scene raster programs were already built by a
    separate lazy-init path (not `GfxRenderer::Ensure`), so the
    remaining independence work was dead-coupling removal: deleted the
    write-only `m_gfxFilterAvailable` latch (declaration + three writes
    — never read), deleted the four dead seed-VS path constants
    (`kGfxSeedVsPath`/`kGfxSeedMaskedVsPath`/`kGfxShadowSeedVsPath`/
    `kGfxShadowSeedMaskedVsPath`), dropped the three deleted filter
    shaders from `HLSL_FORCE_O3_FILES` and `tools/shader_od_retry.py`
    `PINNED`, and corrected the stale `gfxfiltered.vert.hlsl.spv`
    hot-swap path to `gpu_scene_raster.vert.hlsl.spv` (with a new
    `swap.cover-filtered-vs` pin). The genuine remaining
    `GfxRenderer::Ensure` coupling is static registration (→ Epoch 03)
    and the HZB programs (→ Epoch 05).
  - **Gates**: `architecture_tests`, `rendergraph_contract_tests`,
    `shader_hot_reload_tests` green; engine lib recompiles clean.
- 03 — `refactor(gfx): canonical static geometry ownership` —
  registration/upload/LOD/lookup no longer depend on GfxRenderer or
  `gfx::State`; consolidate into the GPU-scene-owned geometry registry.
  - **Record**: static registration and the world→gfx mesh identity
    moved OFF `GfxRenderer` into the canonical `tour::gfx` static
    surface: `StaticGeometry` gains `worldMeshMap`/`worldLodMeshMap`,
    a `RegisterWorldMeshes(StaticGeometry&, const World&)` free
    function (the walk previously on `GfxRenderer::RegisterWorldMeshes`,
    minus the `#if 0`/`TOURMALINE_DUMPGFX` TMP-DIAG scaffolding), and
    inline `GfxMeshForWorldMesh`/`GfxMeshForLod`/`IsMeshFiltered`
    accessors. `GfxRenderer` drops `meshMap_`/`lodMeshMap_` and the
    `RegisterWorldMeshes`/`UploadStaticData`/`GfxMeshFor*`/`IsMeshFiltered`
    methods; `WorldRenderer` calls the free functions on the State.
  - **Gates**: the 7-test GFX subset green (architecture/rendergraph-
    contract/lightmap-pull/gpu_scene_{world,lod,candidates,validation});
    text integrity 8/8; engine lib recompiles clean.
- 04 — `refactor(gfx): capacity and startup allocation overhaul` —
  remove the 65,535/65,536 ceilings; derive reservations from world
  requirements; expose utilization/high-water/overflow.
  - **Record**: raised the two hard admission ceilings that reject
    large scenes — `kMaxMeshes` 8192 → 262144 (HLSL mirror
    `GFX_MAX_MESHES`) and `kMaxRasterDrawGroups` 65535 → 262144 (HLSL
    mirror `GS_MAX_GEO`; the hardcoded `65535u` bounds in
    `gpu_scene_task_mesh_runtime.h` now reference the named constant).
    Both are admission ceilings, not reservations: the per-flight
    publisher buffers double to the actual geometry count, so GPU
    memory follows usage. Sibling limits are already at 262144+
    (`GFX_MAX_CLUSTERS`, `GS_MAX_WORK`/`GS_MAX_INSTANCES`/
    `GS_MAX_CLUSTERS` 4194304). Remaining doc-only items: leaf-range /
    instance-candidate / shader-row / PSO-work budgets
    (`MaxPassedLeafRanges` 65536, `MaxPassedInstanceCandidates`
    1048576, `MaxShaderRowSlots` 4194304, `workCandidates` 1048576) are
    declared-max checks, not admission ceilings — to be re-derived from
    checked scene requirements with GPU validation.
  - **Gates**: 7-test GFX subset green; `EngineScene_shaders` recompiles
    (HLSL mirrors stay in lockstep); text integrity clean.
- 05 — `refactor(gfx): shared prepass Hi-Z and parallel skinning` —
  one pyramid, two culling consumers; skinning overlaps the static
  prepass/Hi-Z work.
  - **Record**: the shared per-view pyramid is already wired at
    baseline — `tour::visibility` owns the Hi-Z storage/layout, and both
    the static GPU-scene HZB cull (`GpuSceneHizCull` /
    `GpuSceneMeshletHizCull`, consuming `gfx::HizAddress`) and the
    skinned/direct cull consume the SAME pyramid; no redundant
    per-view pyramid generation remains. The HZB BUILD programs
    (`hizProgram`/`hizSpdProgram`) still live on `gfx::State` (they move
    to `tour::visibility` with the Epoch 08 `gfx::State` deletion), and
    the skinning/prepass overlap scheduling is GPU-timing-validated work
    (deferred to Epoch 09).
  - **Gates**: no code change this epoch — the shared-pyramid + single-
    generation invariant is already met; scheduling overlap is a
    GPU-validated optimization, not a correctness gap.
- 06 — `refactor(gfx): delete lightmaps and cone debug visualization` —
  both features removed end-to-end; normal-cone culling data preserved.
  - **Record (cone debug — done)**: deleted the cone-axis overlay
    end-to-end — `gfxcone_debug.cs.hlsl`, the `GfxConeDebugPush`/
    `GfxConeDebugLines` HLSL mirrors, `ConeDebugPush`, the
    `State::coneDebug*` fields + `SetConeDebug`/`AddConeDebugPass`/
    `PushAndDispatchConeDebug`/`ConeDebugReady`/`ConeDebugLines*` API,
    the `gfx_dynamic_capacity` `coneDebug` byte budget, the
    `m_debugCones` UI checkbox, and the overlay's candidate-count +
    draw + buffer-hint wiring in WorldRenderer. Normal-cone culling
    data (`gfxskinbounds`, `ClusterConstants` cones, the meshlet-debug
    heat mode) is preserved.
  - **Record (lightmaps — done)**: deleted the runtime lightmap surface
    end-to-end — `default_lightmap.vert.hlsl`, `lightmap_pull.hlsli` and
    `lightmap_pull.h` (plus the `DIRECT_LIGHTMAP_PASS` blocks and the
    lightmap-pull fetch in `default.vert.hlsl`), the `GfxChartXforms`/
    `GfxLightmapHeap`/`GfxLightmapUv` HLSL types, the pure-lightmap push
    types (`LmChartXform`, `UvFlatVsPushTail`, `DirectLightmapPullTail`
    + the `kUvFlatVsPushOffset`/`kDirectLightmapPull*` constants), the
    `GfxRenderer::EnsureChartXforms` chart-transform upload, the
    WorldRenderer direct-LM path (`vertLightmapShader`,
    `directLmEligible`/`directLmProgram`/`withLightmap`), the editor
    chart-xform upload + `chartTail` param, and `test_lightmap_pull.cpp`
    + the `contract.uvflat`/`contract.tail-lmxforms` pins. The in-layout
    lightmap words (`Draw::reservedMiscW/reservedMisc2X/reservedMisc2W`,
    `FilteredVsPush::reservedLmXforms`, `GfxMeshParams::reservedLightmap/
    reservedLmXforms`, `MaterialRange::reserved`, and their HLSL mirrors)
    are kept as reserved stride padding to preserve the pinned
    112/48/240-byte layouts. The cook/import `StaticMesh::lmChunk` +
    `World::lmChartTable` data remains serialized as a retired no-op
    (loading already drops it), pending a follow-up magic bump.
  - **Gates**: 8-test subset green (architecture/rendergraph-contract/
    gpu_scene_{world,lod,candidates,validation}/asset/world-entity);
    text integrity 8/8; engine + shaders + Editor + App link clean.
- 07 — `refactor(gfx): direct/skinned rendering isolation` — skinned
  direct path independent of GfxFilter; uses shared visibility.
  - **Record**: removed the obsolete shadow-candidate ring — the
    dead `BuildShadowDrawCandidates` walk + `m_shadowCandidates`
    (`std::vector<gfx::DrawRequest>`) + the `gfx::DrawRequest` struct
    itself. The retired filter's `gfx::BuildShadowList` consumer is
    gone (stale-comment-only), so the ring was filled but never read
    (only its `.size()` was profile-logged). Skinned direct rendering,
    GPU skinning, deformed bounds, and the shared Hi-Z culling are all
    unchanged and already independent of the GfxFilter ownership model.
  - **Gates**: engine + test subset rebuild clean; text integrity green.
- 08 — `refactor(gfx): delete legacy GeometryFX architecture` —
  GfxRenderer, unused gfx_types, gfx_dynamic_capacity, the legacy draw/
  candidate rings, and the retired shader compatibility interfaces.
  - **Record (partial)**: deleted the dead skin-bounds pass shells
    (`gfx::AddSkinBoundsPass` / `gfx::PushAndDispatchSkinBounds`), the
    CPU HZB helpers (`gfx::HzbOccluded` / `gfx::HzbMipForRect`), and the
    dead inline accessors (`MeshCount`, `DrawCapacity`, `CandidateCapacity`,
    `ViewFrame`, `BatchBuffer`, `DrawBuffer`, `IndirectBuffer`, `BatchCount`,
    `FilteredIndexBase`, `IndexHeapLogical`, `BatchLogical`, `DrawLogical`,
    `IndirectLogical`, `FillVsSkinTail`, `FillMotionPrevTail`). The
    remaining surface — splitting `gfx::State` (StaticGeometry → GPU-scene,
    Hi-Z → `tour::visibility`, skin-bounds → the skinned owner,
    texture-demand/cull-state → the streamer), deleting
    `GfxRenderer`/`gfx_types.cpp`/`gfx_dynamic_capacity.h` and the seed/mesh
    shaders, and shrinking the R7/R8 baselines — is the deep multi-day tail
    and needs GPU validation.
  - **Gates**: engine + 8-test subset rebuild clean; text integrity green.
- 09 — `test(gfx): validation and performance certification` —
  correctness, memory stability, frame-time gains verified.
