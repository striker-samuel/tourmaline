# AGENTS.md

## Runtime layers (app/src — the project façade stack)

- **ProjectRuntime** (the façade): stable ids → appmanifest [ids] rows →
  pak bytes. `ReadAsset(id)` serves TEXTURE, SOUND and SHADER payloads
  (the three VFS-addressable pak kinds); scenes load by identity, meshes
  ship inside their scene's cluster. `OpenPak`/`AdoptPakBackend` = the
  shared lazy backend cache (one image per archive whatever opens it).
  `AttachCompiledCatalog` (row-level + catalog-fingerprint checks) is
  the compiled-catalog gate project hosts run.
- **DeployCheck** (`app/src/DeployCheck.h`): the DEPLOYMENT VALIDATION
  GATE, pre-graphics in the `--project` launch — every manifest-listed
  pak EXISTS + opens (PakReader magic/CRC) + its CONTENT-ADDRESSED
  name (`<stem>-<fnv1a64>.pak`) matches the file's ACTUAL bytes (a
  stale/wrong pak under the expected name fails even though its own
  image is internally valid), and an ENGINE SHADER SOURCE exists
  (`smvk::shaders::HasAnySource` — the engine-shaders.pak package or
  the loose dev tree). A broken deployment is a loud, actionable
  failure before any Vulkan/window work, never a half-booted app.
  Unit-tested in test_pak_host.cpp (`deploycheck.*`).
- **ProjectScenes** (docs/project_scenes.md): the scene-INSTANCE layer —
  `LoadScene`/`LoadSceneAdditive`/`RemoveScene` by NUMERIC id over a
  live World (SceneHandle = 64-bit generational, same shape as the
  gameplay handles), the id→cooked-scene join ([ids] scene rows carry
  the cooked NAME as their 5th token — the build records it), and the
  OWNERSHIP-AWARE prefetch (Prefetch/Release pins + the per-scene asset
  registry; `RemoveScene`'s `releasedAssets` out-param reports the
  dropped ids the decoded-form caches evict). The GPU-less tests drive
  it through the public `clusterLoader` seam.
- **AudioRuntime** (docs/audio_source.md): the scene-aware audio layer
  (an IAppElement) — clip cache keyed by sound id, emitter states keyed
  by the STABLE (cluster, cluster-node) pair through
  `World::clusterMergedNodes`, per-frame distance attenuation +
  constant-power pan, autoplay, scene/spawn lifecycle. The imperative
  surface (PrefetchSound/PlaySound/StopSound/SetAudioSourceVolume/
  SetListener) forwards through its singleton. The mixer itself stays
  amplitude+pan-only (`SetVoiceSpatial`).
- **Sound assets are a first-class kind**: `PakAssetKind::Sound` (raw
  .wav at the VFS path, served by PakBackend), 0x4xxx content ids
  (scenes 0x1xxx / textures 0x2xxx / meshes 0x3xxx / sounds 0x4xxx),
  and the cooked scene's v18 audio section stores NUMERIC sound ids
  (the pak cooker's `--sound-id` map resolves paths; a non-empty map
  with an unresolved referenced path FAILS the cook — never silently
  omit). The appmanifest's kind gate is `scene|texture|mesh|sound`.
- **The gameplay-handle model** (engine Engine/Scene/entity_handle.h):
  NO generic game-entity registry — gameplay structs compose TYPED
  64-bit generational handles (MeshEntityHandle/AnimatorHandle/
  AudioSourceHandle, zero = invalid, generation bumps on destroy).
  The generated project.h carries TYPED KEYS (SceneKey/TextureKey/
  MeshKey/SoundKey constants — which ASSET; handles name which
  INSTANCE). The full mesh-entity + animator surface has landed
  (docs/entities_and_animators.md): World::SpawnMeshFromNode/
  DestroyMesh (cross-cluster bindings as stable (cluster, pool
  handle) pairs, the resolve tail RE-MIRRORS the rows),
  World::AddAnimator + the Animator facade (Play/CrossFade/SetSpeed/
  Pause/Resume/Stop, per-entity private joint arrays, the
  World::Update flush through Cluster::poseExternallyDriven — the
  renderer frame build drives Update with the frame's dt, NEVER a
  second caller). Renderer limit: one POSE per cluster (independent
  STATE for all animators); the per-draw palette table + the
  SpawnMesh(MeshKey)/LoadMesh/LoadSkeleton/LoadAnimation asset surface
  are the documented follow-ups.

## Build system

**Always configure and build with the Ninja generator.**

```powershell
# Configure (Release)
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .

# Build
cmake --build out/build/x64-Release --target App

# Configure (Debug)
cmake -B out/build/x64-Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .
cmake --build out/build/x64-Debug --target App
```

Never use the Visual Studio generator — the build directories in `out/build/`
are Ninja-configured, and mixing generators fails with "generator : Ninja does
not match the generator used previously". If a `CMakeCache.txt` mismatch ever
appears, delete the offending `out/build/<config>` directory and reconfigure
with Ninja.

MSVC environment: build from the "Developer PowerShell for Visual Studio"
(already initialized), or use `build.bat` (reuses an initialized shell,
else finds VS via vswhere and calls its vcvarsall). From any other plain
shell, call YOUR install's vcvarsall first — no path is universal, adjust
the version/edition to the machine:

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul && cmake --build out/build/x64-Release --target App"
```

## The generated project.h (content as code)

- **Project identity (persistent, never name/path-derived)**: the
  `project.tourproj` marker carries a `guid=` row (format version 2)
  minted ONCE at project creation; the stable 64-bit
  `project::ProjectId` derives from that GUID
  (`smvk::project::DeriveProjectId`), so renaming the project or
  moving the root keeps every compiled-in id valid. Current-format
  policy — NO v1 marker reader: a marker without a valid guid is
  rejected by `Project::Open`, the editor's `OpenProject` and the
  launcher's scaffold; `Project.exe init` on an existing project is
  the ONE explicit adopt-and-mint recovery path. The editor, launcher
  and CLI agree on the same parse.
- `project.h` (the `tourmaline_add_project`/`tourmaline_attach_project`
  layer, `cmake/TourmalineProject.cmake`) is a GENERATION-STAMPED,
  self-contained C++ catalog: `project::Generation` / `GeneratedAt` /
  `ContentHash` constexpr (cheap version control for asset time) plus
  an EOF machine-readable COMMENT section (`TOURMALINE-METADATA`,
  versioned markers) carrying the per-asset/per-pak fingerprints
  (mtime_s/size/fnv1a64 hash + pak cook-state) — the generation
  ledger lives IN the header, no side file. Never code against the
  EOF section; it is tool data. The fingerprints are VERIFIED CONTENT
  HASHES, recomputed from bytes on every run — matching size + mtime
  is NEVER proof of unchanged content (preserved-timestamp, backdated
  and same-size edits are detected; the mtime/size ride the records
  as diagnostics + the demotion-only cook-state hints).
- The stamp moves ONLY on a generation EVENT (any change to the
  emitted text — catalog semantics or fingerprints — or a forced
  refresh); an unchanged generation re-emits byte-identical text, so
  the build-time `<Name>_ids` refresh never recompiles includers.
  `<Name>_stamp` (`--stamp`) forces a new generation every run;
  `<Name>_paks` cooks content WITHOUT building C++ and re-stamps the
  header (the recipe build is the one writer that knows pak content —
  its pak records go `state=verified`; a catalog-only refresh can
  never upgrade a state, only preserve or demote to `stale`/`missing`).
  ContentHash covers the catalog SEMANTICS only — a texture repixel
  keeps it; the EOF asset record carries the new file hash.
- The three operations are independent: `--target <Name>_ids`
  (catalog only), `--target <Name>_stamp` (forced generation),
  `--target <Name>_paks` (cook, no C++), `--target <Game>` (exe + the
  auto refresh). Unit-tested in test_project_pipeline.cpp
  (`stamp.*`, `identity.*`, `proj.*`).
- **`Project.exe explain`** (the provenance/stale-causes CLI):
  `explain --project <dir> --recipe <file> --id <0xhex id | path>
  [--pak-dir <dir>] [--header <project.h>]` reports one asset's full
  chain — identity (id/symbol/kind/guid), ownership (the pak + its
  declared dependencies, each proven earlier in the topological plan
  order; recipe cycles/missing deps fail loud with the resolver's
  diagnostics), freshness (the VERIFIED source hash vs the last
  generation's EOF record) and deployment (the published
  content-addressed file + its cook state). Retired identities report
  the tombstone reservation + the restore path. Like the ids pass it
  reconciles the registry to the recipe. Smoke-tested in
  test_project_pipeline.cpp (`catalog.explain-*`).
- **The retired asset-ID/tombstone contract**: deleted assets
  TOMBSTONE in assets.db (active=0 — the id+symbol stay reserved
  FOREVER, never reused; a new asset at the same path gets a fresh id
  + collision-suffixed symbol). The generated header emits the
  RETIRED table: `namespace retired` carries the historical TYPED
  symbols with their values preserved (stable API compatibility) +
  `RetiredIds` and the compile-time active-membership queries
  `IsRetired(Id)` / `IsActive(Id)`; retired entries are excluded from
  the active namespaces, the Assets array and the published
  manifests, and a runtime load of a retired id fails explicitly (no
  compiled row). The retired set is catalog SEMANTICS (Compute-
  CatalogContentHash folds it — a retire/reactivate is a generation
  event). RESTORING the original asset GUID re-activates the SAME
  id+symbol (the identity is the GUID, never the path). Unit-tested
  in test_project_pipeline.cpp (`catalog.retired-*`,
  `catalog.reactivate-*`).

## Source identity + the file watcher (the watched-resource pipeline)

- **The glTF identity ledger** (item: identities survive reimports):
  the `.gltf.meta` manifest is the family's identity LEDGER, fully
  separate from the source-byte fingerprints. The family guid comes
  from the caller's explicit resolution (`ImportProjectAsset` resolves
  the PUBLISHED manifest before staging and passes it) or the previous
  manifest (identity CARRIES across content edits — the digest records
  the change) or the deterministic from-zero mint (`SourceAssetGuid`).
  Subasset identities (materials/anims/geometry) carry BY NAME (the
  logical key) — a DCC re-export that reorders the glTF arrays keeps
  every GUID where cooks, `.tour`s and content ids expect them; a
  name-group COUNT change against a previous record is AMBIGUOUS and
  the import FAILS (never a silent re-assignment); removed names
  retire. A manifest that exists but does not parse (or carries a
  malformed guid) refuses the derive — identity repair is explicit.
  Unit-tested in test_project_pipeline.cpp (`identity.*`).
- **The project-scoped native filesystem watcher**
  (`Engine/Utils/file_watcher.h`): one background thread over
  REGISTERED watch roots (recursive dirs + single files),
  ReadDirectoryChangesW over an IOCP on Windows / inotify on Linux.
  The POLICY (ProjectDefault) excludes generated artifacts/
  intermediates/published products whatever directory they sit in
  (derived `.geometry`/`.material`/`.anim`, `.spv`, `.pak`/`.tapp`,
  staging/`.tourmaline`/`out`/`build`/`paks` trees, the generated
  `project.h`, atomic-save temps) — only authoritative sources produce
  events (`.meta` sidecars INCLUDING the `.gltf.meta` ledger ARE
  watched). Changes settle after a quiet window + a readability probe
  (atomic-save renames land as ONE Modified on the target);
  `Overflowed()` reports lost OS events and `Reconcile()` is the
  stat-only snapshot diff (silent first baseline; missed events
  during a stop/restart report on the next call). Poll() once per
  frame is cheap — the watcher never scans/hashes the whole project
  per frame. Unit-tested in test_file_watcher.cpp.

## Submodules

Unmodified upstream deps are pinned git submodules under `external/`
(nanosvg, volk, SPIRV-Reflect, glm, meshoptimizer, DirectXTex, json,
JoltPhysics). After
cloning or pulling: `git submodule update --init --recursive` (or clone with
`--recursive`). The rest of `external/` is vendored content (trimmed/patched
trees, binary packages — see `THIRD_PARTY_NOTICES.md`).

## Shaders

- Source: `engine/shaders/` (HLSL, compiled by DXC via `cmake/CompileShaders.cmake`)
- Output: `out/build/<config>/content/shaders/*.spv`, synced next to `App.exe`
- Editing a shader: just rebuild the `App` target — the `EngineScene_shaders` dependency
  recompiles changed `.hlsl` and syncs the `.spv`
- HLSL entry points: `mainVS`/`mainPS`/`mainCS` by file extension
  (`.vert.hlsl`/`.frag.hlsl`/`.cs.hlsl`)
- Debug builds compile shaders `-Od`; files listed in `HLSL_FORCE_O3_FILES`
  (`cmake/CompileShaders.cmake`, currently `gfxfilter.cs.hlsl` — its `-Od`
  SPIR-V AVs nvgpucomp64.dll) are pinned to `-O3` even in Debug

### Shaders as packaged assets (the engine shader library)

- `PakAssetKind::Shader` (kind 12) stores compiled bytecode VERBATIM as
  a blob keyed by its VFS path (`content/shaders/<file>.spv` — the
  path strings consumers always used); it is the THIRD
  VFS-addressable kind (with Texture and Sound):
  `PakBackend::Exists/TryRead/ListDir/Stat` all serve it.
- `smvk::shaders` (`Engine/Utils/shader_library.h`) is THE unified
  shader reader: one process-wide overlay mounting (in precedence)
  loose DEV trees (the exe-relative `content/shaders/` the build
  syncs — a recompiled shader serves immediately in dev; explicit
  `MountLooseDir` overrides too) over SHADER PACKAGES
  (`MountPak`/the exe-relative `engine-shaders.pak` — the production
  source). Every read VALIDATES the bytecode magic (SPIR-V on the
  Vulkan backend; `DetectBytecode` also distinguishes DXBC/DXIL) — a
  corrupt blob fails at READ time with a named provider, never inside
  a driver. `Read(path)` is a drop-in for the old
  `readFile("content/shaders/...")` reads. Hosts call
  `smvk::shaders::Init()` before graphics initialization (app +
  editor do); every entry is lazily self-initializing anyway.
- `engine-shaders.pak` (the ENGINE-OWNED package) is built by
  `EngineScene_shaderpak` (`tools/pak_cooker/CMakeLists.txt`) from the
  AUTHORITATIVE DECLARED compiled-output list
  (`engine-shaders.list`, written by `engine/CMakeLists.txt` from
  `compile_shaders()`'s exact `.spv` set — base outputs + every
  configured variant, whatever the backend/build produced; a retired
  variant's stale `.spv` in an old build tree NEVER ships). Pak.exe's
  `--shaders-list` mode validates every blob's SPIR-V magic and packs
  the list exactly (`--shaders <dir>` walks a tree instead). Hosts
  deploy the package next to their exe (`HostSetup`) — a MANDATORY
  runtime dependency of every graphics host, built independent of any
  scene or project recipe.
- Regression-tested in test_shader_library.cpp (the pak round trip,
  bytecode detection, validation failures, dev-override precedence,
  the declared-list discipline, corrupt-declared failures).
- **PROJECT-AUTHORED shader assets** are a separate identity lane:
  the recipe resolver classifies `.spv` under the project's own
  content tree as `PlanAssetKind::Shader`; asset-only paks embed them
  VERBATIM as Shader-kind blobs through the content-set path (SPIR-V
  magic validated at pack time — corrupt blobs fail the build); the
  content-id range is **0x5xxx** (`assets.db`'s `IdRangeForKind`),
  the generated project.h carries `project::shaders::*` typed
  `ShaderKey` constants + `AssetKind::Shader = 4` (the ABI kind byte
  — appended, nothing renumbered); the `.tapp`'s [ids] rows take
  `kind shader` and `ProjectRuntime::ReadAsset` serves the raw
  bytecode. The ENGINE-NATIVE set (engine-shaders.pak) is build-owned
  — recipes never control its inclusion. Unit-tested in
  test_project_pipeline.cpp (`shaders.*`).

## Running

`App.exe` runs from the build directory (`out/build/x64-Release`), loading
`content/paks/*.pak` and `content/shaders/*.spv` relative to CWD.

Headless-ish deterministic capture (auto-exits after the bench budget):

```powershell
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

- `TOURMALINE_CAPTURE=<frame>` exports `exports/screenshot_*.png` — the frame
  number must land INSIDE the bench budget (capture < bench; e.g. capture 12
  with bench 32), otherwise the auto-exit happens before the export
- `TOURMALINE_SSR_DUMP=<frame>` dumps SSR stage stats + PNGs to `exports/`
- `TOURMALINE_AO=off|ssao|sao` pins the AO mode headless (default sao; the O
  key / UI combo stay live); combined with `TOURMALINE_SSR`,
  `TOURMALINE_EVSM`, `TOURMALINE_TRANSPARENCY` this spans the full A/B matrix
- `TOURMALINE_GI_INTENSITY=<0..2>` pins the GI intensity slider headless
  (scales the baked-GI ambient and the Sky IBL fallback; 0 leaves direct
  sun only, 1 is authored, 2 doubles it)
- `TOURMALINE_NORMAL_FIX=0|1` pins the JHFAA-style shading-normal validity
  correction headless (experimental, default off — the silhouette
  reflection-vector constraint; docs/jhfaa_normal_fix.md);
  `TOURMALINE_NORMAL_FIX_DEBUG=1` is the red/orange violation overlay
  (red = back-facing shading normal, orange = reflection below the
  geometric horizon — works with the correction on or off)
- `TOURMALINE_VOLUMETRIC=0|1` forces the raymarched fog on/off headless
  (scene fog settings lose); `TOURMALINE_VOLUMETRIC_STEPS=<4..128>` pins the
  march steps for heavy fog benches; `TOURMALINE_VOLUMETRIC_GI_VOLUMES=0` is the
  fog-GI quality bypass (NO convex-volume walks — the world-field/sky fog-GI
  base still samples; only the volume composite is skipped — pairs with
  `TOURMALINE_VOLUMETRIC` for volume-walk cost A/B); `TOURMALINE_FOG_GI_DEBUG=2`
  is the transmittance view (throughput as grayscale — the optical-density
  diagnostic: white = no extinction, black = complete)
- `TOURMALINE_PROBE_PREMIX=0|1` pins the Remember Me probe pre-mix
  (docs/probe_premix_plan.md; default on): `1` = the per-frame GPU weights
  + mix passes (a blended cubemap + a GPU-owned selection; the material
  does ONE fetch + dominant-box parallax), `0` = the per-pixel walk
  fallback (≤8 fetches + per-pixel box parallax). `0.00%` differing pixels
  is the A/B contract between the two
- `TOURMALINE_FP_MASKS=0|1` pins the Forward+ HIERARCHICAL TILE-LIGHT
  CULLING (FFXVI "Shadow Techniques" 2.1–2.3; default on): `1` = the
  DEFAULT forward+ tile cull consumes the phase-1 artifacts — lights
  CPU-sorted by near distance (DsSortLightsByNearDistance, now run for
  the F+ path too, not just DS), zBins (ranged min/max, two-lookup depth
  windows) + 64×64px coarse tile masks (132 B/group, 1024 light bits)
  uploaded per flight to the ForwardPlus builder's own rings
  (forwardplus.h kFp*; kDs* alias the same constants); the cull
  intersects coarse mask × zBin window at the 32-bit word level
  (DsWindowMask) and runs the exact side-plane/slab tests only on
  surviving bits. `0` = the linear scan fallback (the A/B reference).
  NO-MISSING-CONTRIBUTOR is the A/B contract (the paper's own
  invariant): the zBin windows are conservative supersets of the exact
  depth cuts, and each coarse-mask reject (group plane / projected rect)
  PROVES zero contribution to the tile's pixels — so captures match the
  reference except in SATURATED tiles, where the mask path keeps a
  superset of the reference's true contributors (the reference's sorted
  insertion can waste slots on the linear scan's zero-contribution false
  positives — center-based -range side-plane tests do not compose across
  nested wedges for behind-camera centers). The fog track uses its own
  zBin window ([0, fogMaxDistance] — march samples live outside the
  surface slab; the slab window would drop fog lights). Probes ride a
  linear tail (the phase-1 builders run lights-only). Invariant
  unit-tested in test_forwardplus_masks.cpp. Under deferred shadows the
  F+ cull keeps the linear scan (the DS ring owns the artifacts; the F+
  light leg is dead work there)
- `TOURMALINE_HZB_SPD=0|1` A/Bs the SSGI MAX-HZB build (default 0 =
  the per-mip dispatches): `1` = the single-dispatch SPD build
  (hzb_spd.cs.hlsl — same BDA buffer, same mip offsets, byte-identical
  output). `TOURMALINE_HZB_SPD_VERIFY=1` builds BOTH from the same depth
  (per-mip live + SPD shadow) and byte-compares every mip post-Execute
  (stalls; the validation harness — 0 mismatching texels is the
  contract). Extents > 4096 always fall back to the per-mip path
- `TOURMALINE_HIZ_SPD=0|1` / `TOURMALINE_HIZ_SPD_VERIFY=1` are the same
  A/B + verify pair for the gfx filter's CULLING MIN-HZB (the
  TOURMALINE_HIZ family; the `.min` twin of hzb_spd.cs.hlsl, per-view
  per-flight counter ring). The verify compare reports as
  [GfxHizSpdVerify]; both pyramids build from the same seed depth
- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off - uncapped immediate present; an explicit `--present <mode>`
  wins)
- `TOURMALINE_CONTACT_SHADOWS=0|1` pins the sun contact shadows on/off
  headless (contact_shadow.h - the pre-Main depth raymarch toward the
  sun, min'd with the CSM/EVSM factor in the lit body; the panel
  checkbox stays live). `TOURMALINE_CONTACT_SHADOWS_STEPS=<4..32>` pins
  the march samples; `_DIST=<0.05..2>` the march length (m);
  `_THICKNESS=<0.005..0.2>` the acceptance band (m); `_RES=1|2` the
  visibility resolution (1 full, 2 half - the A/B to benchmark, half
  saves tracing but can lose thin occluders). Defaults: 0.35 m /
  0.03 m thickness / 0.01 m bias, 12 steps, half-res.
  `TOURMALINE_CONTACT_SHADOWS_DUMP=<frame>` one-shot dumps the visibility
  texture's stats + an ASCII occlusion map to stderr (the FOG_TILE_DUMP
  discipline - sync download, debug runs only).
  `TOURMALINE_DEBUGVIEW=contact` selects the contribution view (the
  Debug-view combo's "Sun contact shadows (contribution)"): R = the
  contact march's visibility, G = the CSM/EVSM factor - green = the
  march's contribution, red = the march missed what the cascade caught,
  dim blue = NdotL<=0 (inert), magenta = the pass not running. The
  contact pass stays LIVE under this view (it samples the real march).
- `TOURMALINE_DEBUGVIEW=ssgi_confidence|ssgi_raw|ssgi_layer|
  ssgi_baseline|ssgi_contribution` selects the SSGI compose debug views
  (the ViewMode Ssgi* combo entries; the chain must be on -
  TOURMALINE_SSGI=1). The four HDR-scale views (raw/layer/baseline/
  contribution) write real radiance through the frozen scene exposure,
  so they carry a DIAGNOSTIC +EV GAIN - the Debug panel's "SSGI debug
  exposure (+EV)" slider, default +4 (x16), headless pin
  `TOURMALINE_SSGI_DEBUG_EV=<0..12>` (shared across the four so
  switching preserves relative brightness; the confidence view
  self-normalizes via 1/exposure and ignores it). Background pixels
  read BLACK under every SSGI view (no diagnostic signal exists in
  the sky).
  Per-material receive gate: MATERIAL_RECEIVE_CONTACT_SHADOWS (the
  MaterialValues::emissiveFactor.w flags word, mesh.h kMaterialFlag*): the
  SMVK_materials_contactShadows glTF extension {"receive": false} opts out
  at import, the editor material panel's Contact checkbox + the setMatAttr
  contactShadows tour attr at runtime. OPT-IN bit - paks cooked before the
  flag read w=0 and receive NOTHING (recook)
- `TOURMALINE_DEBUGVIEW=pbrhealth|pbrbase|pbrdiffuse|pbrrough|pbrmetal|
  pbrf0|sss|hair|water` selects the PBR Material Validation Debugger
  views headless (the Debug-view combo's "PBR / ..." entries): the six
  validation/readout FSs over the SHARED material evaluator
  (engine/shaders/material_evaluation.hlsli - the lit body's exact
  material-input math, raw pre-clamp values retained). Material Health =
  the combined heatmap (green passes the configured checks, yellow
  suspicious - albedo luma/dielectric edge, intermediate metallic,
  roughness below the renderer's 0.04 floor; red out-of-range/NaN input,
  magenta non-finite in the value views). pbrbase/pbrdiffuse/pbrrough/
  pbrmetal/pbrf0 are the flat value readouts (the 1/exposure debug
  pre-scale means intensities are display-only, not numeric). sss/hair/
  water are NAIVE shading-model previews (development test beds - d'Eon
  transmission SSS, Kajiya-Kay hair, Fresnel+wave water; sun/sky shaded
  like the POM view, deliberately first-cut, owner develops further)
- `TOURMALINE_DEBUGVIEW=clipping|clipdark|clipbright` selects the
  CLIPPING views (ViewMode 30..32, combo entries "Clipping ..."):
  IMAGE-LEVEL diagnostics in the tonemap pass (tonemap.frag.hlsl's
  debugView 2..4 branch) - the FINAL mapped+graded image (after
  exposure/bloom/tonemap/LUT) is annotated, BLUE = display-encoded SDR
  luma at/below the shadow threshold (crushed shadows), RED = at/above
  the highlight threshold (clipped highlights), in-range pixels keep
  their normal color. A NEAR-CLIPPING warning, not proof of irreversible
  loss (legitimately black/white surfaces flag the same). The post-fx
  chain + exposure metering stay LIVE under these views (the
  DebugVisualizationActive/ExposureMeteringActive exclusions) - otherwise
  the view would inspect a different image than the player sees.
  Thresholds: the Debug panel sliders (shadow 0..0.1, highlight 0.9..1.0,
  defaults 0.01/0.99) or the `TOURMALINE_CLIP_DARK=<0..0.1>` /
  `TOURMALINE_CLIP_BRIGHT=<0.9..1>` headless pins
- `TOURMALINE_GI_POINTS=1` arms the GI PROBE SPHERES (the "Probe
  points" Debug-panel checkbox's headless twin; Rendergraph/giprobe.h -
  the ImGui probe-dot overlay's 3D successor): every convex-GI-volume
  probe renders as an instanced 0.12 m 42-vertex icosphere (one
  DrawIndexed instanced call per volume over a static per-volume
  instance table - position + integer grid coords, built once on the
  first arm, strided to the old 6000-probe ImGui budget) whose VERTEX
  shader Loads the probe's EXACT texel from the volume's 7 SG layers
  (Texture3D.Load, never the filtered Sample) and evaluates
  SgGiDiffuseIrradiance against the sphere direction - each sphere is a
  tiny irradiance ball showing the baked field's directional answer at
  that probe (FS = pure HDR pass-through). The probeball slot contract:
  the HDR tail (bloom/tonemap grade the spheres), scene depth TEST-ONLY
  (occluded probes hide behind geometry). The camera-distance filter
  (the old overlay's, slider-adjustable) runs GPU-side as a degenerate-
  instance cull in the VS. Gates on (m_giVolumesLoaded &&
  m_giVolumesEnabled) - the same condition the FrameUniforms
  giVolumeLayers slots publish under
- AUTOMATIC GI PROBE-GRID LAYOUT OPTIMIZATION (cubemap_volume.h/.cpp:
  ChooseGiVolumeGridLayout): a BVH-only candidate search (NO path
  tracing) over spacings x lattice phases, scored on invalid probes
  (clearance + a ray-parity SOLID-INTERIOR test - ClosestDistance is
  unsigned and cannot tell inside-solid from near-surface), visibility-
  aware surface coverage (area-weighted surface witnesses need a valid,
  UNOCCLUDED probe among their cell's 8 corners - coverage weighted 10x
  heavier than invalid probes per the contract), and probe count/
  memory. Candidates exceeding kGiVolumeMaxDim are REJECTED, never
  silently truncated. Authored on the volume (SceneGiVolume + the
  editor's inspector, all persisted as setGiVolumeAttr tour verbs):
  `spacingLow`/`spacingHigh` DENSITY GATES bound the bake-time search
  (0 = off, the default +-20% around the preferred spacing when the
  editor button runs), and the `optlayout` SEED (one verb: number =
  spacing + vec4.xyz = phase) is the editor's "Optimize grid layout"
  button result - committed as ONE undoable step and consumed verbatim
  by BakeGiVolume (seed > gate search > the LEGACY fixed layout, so
  every pre-optimization volume bakes bit-exact). The button runs on
  the LIVE editor world (NO tour snapshot, NO replay, NO cook:
  ExtractTraceSceneFromWorld snapshots the geometry from the resident
  clusters' retained CPU uploads on the main thread - unsaved edits
  included - then the fibers lane runs Finalize (the BVH) +
  ChooseGiVolumeGridLayout over the moved soup; BuildTraceSceneFromWorld
  is the SAME shared assembly as the GiBaker's cluster path, pinned by
  the trace.world.* parity tests in test_gi.cpp); the chosen layout
  also drives the J lattice preview (the seeded
  preview is exactly what the bake builds). The bake-input hash folds
  the gates + seed (stale bakes refuse). Unit-tested in test_gi.cpp
  (glayout.*: gates, cap refusal, coverage contract, parity)
- GI HOT-SWAP (the live-authoring milestone - edit volume -> Bake GI ->
  the viewport updates WITHOUT recreating the WorldRenderer):
  LoadGiVolumes is split into the transactional BuildGiVolumeSet (the
  complete volume set built into CALLER-OWNED locals - a failed build
  never touches the live members) + two entries: the init load and the
  public ReloadGiVolumesHotSwap (generation N+1 built in separate
  resources, published by the member swap; the old generation's Holders
  retire through the deferred-destroy queue behind the in-flight
  frames - per-flight FrameUniforms slabs carry their own slots/BDA, so
  nothing a recording or executing frame references is destroyed under
  it). The probe-sphere instance tables invalidate with the swap
  (m_giProbeInstancesBuilt - the grids moved). The editor's bake-
  completion edge calls the hot swap instead of the WYSIWYG relaunch:
  the live view keeps its camera/quality/state, the last valid GI
  renders until the swap lands, and the re-read rides the same bake
  channel the relaunch used (the world VFS sees the fresh manifest).
  Headless frame-safety regression: `TOURMALINE_GI_HOTSWAP=<frame>`
  swaps the generation MID-BENCH (proven on sponza: published at frame
  8, 24 further frames clean, the post-swap capture identical to the
  reference within the noise floor)
- BC6H IS THE CANONICAL GI-volume cooked format ("GV6H" - no flag, no
  toggle, a HARD ENGINE REQUIREMENT): WriteToMemory always emits 9
  BC6H_UFloat per-lobe 3D textures (PackGpuLayersBc6h - per-Z-slice
  4x4 XY blocks, non-multiple-of-4 dims padded with the Frostbite
  valid-texel-average prefill so zeros never expand the endpoint box;
  ~9 bytes/probe vs RGBA16F's 56), compressed at cook time only. The
  device init REFUSES to run without BC6H_UFloat 3D sampled + linear
  filtering (probed at the exact configuration; no fallback by
  design). The reader takes GV6H (blocks verbatim + the lossy
  DecodeBc6hCoeffs mirror for CPU consumers - the audit/Sample/the
  editor) AND the legacy GVOL (old content rebakes; it is also the
  RGBA16F reference form for quality comparisons). The shader
  (sg_gi_volume.hlsli) branches per volume on GiVolumeGpu.
  planeCountBase.z: BC6H = 9 direct .rgb fetches (the amp IS the RGB,
  no unpack - the 7->9 fetch increase is the format's measured cost),
  GVOL = the 7 RGBA16F stream + SgRegularUnpack; slots at
  giVolumeLayers[vol * 10 + i] (the array's original 8x(9+1) shape).
  Both the fast trilerp and the 8-corner weighted paths carry the
  branch. Vis (RGBA8) + the fog mean (RGBA16F) unchanged. On-device
  sponza A/B: 0.03% differing pixels, ZERO strong deltas vs the
  RGBA16F reference; the rebaked pak's volume is 25 KB on disk.
  Round-trip unit-tested (gv6h.* in test_gi.cpp: block math, magic,
  decode tolerance, GpuBytes)
- The probe spheres have three COLOR MODES (the Debug panel's "Probe
  color mode" combo or `TOURMALINE_GI_PROBE_MODE=0|1|2`): 0 = baked GI
  (the white-Lambertian answer E * giIntensity / PI - physical
  radiance, never exposure-scaled), 1 = VISIBILITY (the retired ImGui
  dot overlay's coplanarity check revived in 3D: the probe's min 6-axis
  obstacle distance from the vis layers, exact texel - RED < 0.05 m
  blocked/inside, YELLOW < 1 m near-surface, GREEN open, flat gray = no
  vis data), 2 = the PRE-BAKE LATTICE (a flat marker color, NO bake
  data read - the AUTHORED volume defs' would-be grids from
  scene.giVolumeForNode, cook-parity OBB decode + BakeGiVolume's
  gridMin/dims math via BuildGiProbeLatticeRows; rebuilt every armed
  frame so live editor moves show). The marker modes' colors are
  display-authored and pre-scaled by 1/exposure (the flat-view
  convention - GiProbePush::markerGain); mode 2 needs no FrameUniforms
  BDA. Modes 0/1 gate on (m_giVolumesLoaded && m_giVolumesEnabled);
  mode 2 gates on authored nodes existing. The EDITOR's LIGHTING 'J'
  preview cycle gained a 5th stop ("lattice (pre-bake)") that renders
  the same 3D spheres per viewport (EditorApp::RecordGiProbeLatticePass,
  sub-rect AppendPass offsets) - probe location/density inspection
  before the first bake. BuildGiProbeLatticeRows is unit-tested in
  test_gi.cpp (bake-parity dims/positions + the cap/degenerate edges)
- `TOURMALINE_GEOMETRY_STREAMING=1` gates GPU geometry LOD streaming
  (geometry_streamer.h - v1 stages 1-3 + logical stage-4 eviction,
  DEFAULT OFF): cluster load defers the middle LOD siblings (LOD0 bases
  + each group's coarsest fallback stay resident; skinned siblings
  never defer), the LOD picker selects over the RESIDENT subset only (a
  missing middle LOD degrades to the resident neighbor in error budget
  - the drawn level), and the per-frame tick promotes demand-missed
  siblings (throttled: 16 MiB + 4 promotes/frame) and EVICTS the idlest
  over-budget siblings back to deferred (nothing drawn or demanded
  within the idle window; a level touched this frame never evicts) -
  every rebuild between frames is in-flight-safe (deferred retirement).
  `TOURMALINE_GEOMETRY_BUDGET_MB` pins the promoted working-set ceiling,
  `TOURMALINE_GEOMETRY_EVICT_IDLE` the idle window in frames (the
  headless churn-test pair - small budgets force evict/re-promote
  cycling). Physical sparse backing (actual page release) stays out:
  eviction frees arena RANGES for reuse, the monoblocks keep their
  committed size. Per-cascade LOD demand is RETIRED (owner decision:
  the per-view meshlet/cluster shadow culling already captures the
  savings; cascade LOD floors risk overlap-seam artifacts - see
  geometry_streamer.h's scope note). Stage 6 (disk-range streaming -
  read only the promote bytes from the file instead of pinning the
  whole pak image in RAM) is a clearly-noted TODO in
  geometry_streamer.h: build ONLY when a multi-pak world makes system
  RAM the measured constraint; the addressing/throttle plumbing is
  already stage-6-ready

## Tests

Tests are plain executables (no gtest), gated behind CMake's standard
`BUILD_TESTING` switch (`include(CTest)` in the root CMakeLists - ON by
default). A dedicated tests tree keeps them separable from working configs:

```powershell
cmake -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON .
cmake --build out/build/tests
ctest --test-dir out/build/tests --output-on-failure
```

The normal configs (out/build/x64-Release etc.) build the test targets by
default too — `ctest --test-dir out/build/x64-Release --output-on-failure`
keeps working as-is. Pass `-DBUILD_TESTING=OFF` to exclude the test targets
from a configure entirely (that is what CI does: `.github/workflows/ci.yml`
is BUILD VERIFICATION ONLY; the suite runs in the separate, independent
`Tests` workflow — `.github/workflows/tests.yml`, same Linux/Windows matrix,
the five GPU-window tests excluded on hosted runners: no display / Vulkan
ICD).

## Conventions

- Push-constant structs: C++ (`engine/include/Engine/Rendergraph/*.h`) and
  HLSL mirrors must stay layout-identical — check the `static_assert(sizeof(...))`
  and the HLSL comment when editing either side
- Frame renderer plumbing: `engine::AppFrameContext` (engine/include/Engine/Rendergraph/FrameContext.h)
  carries per-frame handles; renderers snapshot what they need at Append time
- Target-heap packing (designs A + B — the packed render-target memory):
  `docs/target_heap_packing.md` is the system reference (the bind-before-view
  contract, the retirement FIFO pairing, how to read the
  `[TargetHeap]`/`[TransientHeap]` stderr offsets, and `03_fuzz` — the
  regression host). Heap image work must follow the bind-before-view contract
  and the Holder declaration orders documented there
- Heavy explanatory comments are the repo norm — keep the reasoning auditable
- Bake albedo: the CUBEMAP captures bake TEXTURED (per-hit baseColor sampling —
  `GiTraceScene::AlbedoAt`, bilinear REPEAT over decoded texel data capped at
  `kGiBakeTexelMaxDim` 512); the GI fields (.sggi octree + GI volumes) bake
  white-world 0.7 by convention (the 9-lobe SG fit cannot represent per-material
  chroma — verified on Sponza). `--uniform-albedo off` (GiBaker) opts into the
  textured transport for the volume bakes; the invalidation hashes fold the
  albedo MODE + value, so the flip re-traces (stale records refuse). Sponza GI
  flow: `tools/cook/cook_sponza.bat`
  runs Cook → GiBaker (the convex volumes + the global octree from
  `F:/content/models/sponza/Sponza.tour`) → Pak
- Bake transport is FULL PBR (`kBakeAlgorithmRevision` 2): every vertex traces
  the whole glTF metallic-roughness BSDF — the Lambertian diffuse NEE (as
  before) PLUS the GGX specular NEE toward the sun + static spots
  (per-hit metallic/roughness from the MR texel's B/G, F0 =
  mix(0.04, fullBase, metallic)) and one-sample-MIS lobe sampling for the
  bounce hop. A diffuse-only transport treats METAL as black (every specular
  bounce absorbed — Sponza's gold trim ate the sun); white-world runs the SAME
  machinery with its uniform colors (the metal F0 is the uniform WHITE —
  uncolored PBR, chroma-free, the SG convention preserved while the metal
  absorption hole closes). Pinned by TestGiPbrTransport (the 45-degree metal
  mirror redirecting a grazing sun onto a dark floor — zero under a
  diffuse-only transport) + the exact analytic re-pin in
  TestGiPrimarySunIncluded (both lobes' closed forms). Emissive stays
  runtime-only (never baked).
- GI firefly clamp (GiBaker `--clamp 8|off`, both volume bakers): the two-pass
  per-probe cap at clampRatio x the probe's own finite-sample mean luminance,
  pre-SG-fit. The clamp is ALSO a systematic energy loss on spiky distributions
  (a probe whose sky/sun contribution lands in a few percent of its directions
  loses most of that energy — the GI-bake review's finding), so every bake log
  carries the discarded-energy fraction (`clampedEnergyRatio`, "[-N.N% energy]")
  and `--clamp off` is the ready A/B (the invalidation hashes fold the ratio).
  Known transport limitation (deferred, the review's #2): skylight is evaluated
  ON MISS ONLY — no explicit sky sampling with visibility at diffuse vertices,
  so partially-enclosed scenes (Sponza) need deep paths to find openings.
- Bake ray-tracing backend: EMBREE is THE backend (the GiBaker default and the
  engine's GiSceneBuildOptions default). The builtin binned-SAH BVH TRANSPORT
  is DEPRECATED and marked for deletion — `--backend builtin` still works
  (GiBaker prints a loud deprecation warning) and an Embree device/scene
  failure still falls back to it automatically until the removal; the COLD
  non-transport paths (TriangleBvh::ClosestDistance, the placement density
  picks) stay builtin by design. Bakes are reproducible per backend; the two
  backends agree within Monte-Carlo noise.
- Ambient closure (`--cubemap-ambient-closure`, default OFF): captures + the
  GI stack both carrying the ambient double-counts every mirror-like surface
  (the chrome probe ball, gold trim reflect ~2x). ON only for stack-less
  cooks (bistro's bat) where the runtime ambient is the analytic sky fallback.
  Dead captures (mean luma ≤ 20 — embedded/occluded origins, or bounce-tail
  death in deep interiors) drop from the manifest: the closure used to mask
  them; their zones fall to the live neighbors + the GI-occluded sky fallback
- LOD sibling errors stay MESH-LOCAL in the artifact (scale-free) — the
  renderer applies the instance's world scale at pick time
  (`World::SelectLodIndex`). Never bake a scene transform into the serialized
  error (the removed cook-time finalize compounded per re-cook: Sponza's
  0.008 root = 125x/run)
- Weave cloth transmission (`--mask-transmission on`, Cook + GiBaker —
  sponza's bat): MASK cutout cloth keeps its full lit albedo AND passes light
  tinted per texel by its baseColor texture (`GiTraceScene::TransmissionAt`;
  cutout holes pass unobstructed; the tint stays REAL in white-world GI
  bakes). Default OFF — MASK stays the opaque occluder (bistro foliage)
- GPU-counter buffers (anything an `InterlockedAdd`/append reads-modify-writes
  from its first use) MUST be creation-zero-filled via
  `UploadDataToBuffer` (the ssgi-hzb / placement-count convention): DEVICE_LOCAL
  fresh allocations recycle freed pages, so a churn-recreated counter can start
  from a garbage count (the old frozen count landing in the new live slot) and
  the consuming compute shader MMU-faults — the editor's drag-burst
  DEVICE_LOST, pinned via `VK_NV_device_diagnostic_checkpoints`
  (`DumpDeviceFaultInfo` prints the last-reached pass; TOURMALINE_CHECKPOINTS=0
  kills the marker emission; shaders with `-Zi` live in the
  x64-Release-ShaderDebug preset + `TOURMALINE_SHADER_DEBUG=ON` for
  Nsight-Aftermath line tables)
- Dont use F12 for any binding since it can trigger debugbreak
- Dont ever bump format versions or add conversions to keep old data working. Bumping magic to break old files is ok. I have no important assets to keep
- The editor/launcher split: `Editor.exe` REQUIRES `--project <dir>` (no
  project, no launch — the artifact cache/asset registry/import pipeline are
  all project-bound). `EditorLauncher.exe` (editor/launcher/, console v1)
  owns project SELECTION — the shared editor.prefs recents + a typed path
  (scaffolding when the `project.tourproj` marker is missing) — and spawns
  the editor with its pick. The old in-editor blocking project modal is
  retired with the split; smoke/editor runs pass `--project F:/projects/game`
  (see the temp scripts)
- Engine limits are OWNERSHIP-SCOPED (the old global MAX_* block in smvk.h
  is gone). The shared CPU/HLSL layout-contract values (MAX_MATERIALS /
  MAX_SIMULTANEOUS_LIGHTS / MAX_TEXTURES / MAX_VOLUME_TEXTURES /
  MAX_SAMPLERS) live in ONE file — `engine/shaders/engine_limits.hlsli`
  — whose ONLY C++ includer is `Engine/Core/Limits.h` (it converts the
  macros to constexpr and #undefs them; never include the .hlsli from
  C++ anywhere else). Public surface: `engine::renderer::limits`
  (`Renderer/RendererLimits.h` — the GPU material table cap +
  `MaxSimultaneousLights`, the RUNTIME how-many-lights-may-be-BOUND-AT-
  ONCE-per-frame budget, NOT a per-scene/level cap; WorldRenderer's
  FP_MAX_LIGHTS and the legacy smvk::Light ring both derive from it) and
  `smvk::limits` (`RHI/DescriptorLimits.h` bindless budgets +
  `RHI/FrameConfig.h` frame scheduling/window defaults, both re-exported
  by smvk.h). Editing engine_limits.hlsli recompiles every shader AND
  every engine TU on purpose — a layout contract must never ship
  half-applied. The pool↔descriptor budget static_assert lives at the
  INTEGRATION site (vulkan_impl.h, next to bindlessImages_), never in
  Memory/pool.h (the generic pool must not know descriptor budgets).
- Public APIs take RAW POINTERS + COUNT/SIZE, never container types:
  `f(const Thing* items, size_t itemCount)` / `f(void* out, size_t outBytes)`
  — NOT `std::vector`/`std::span`/`std::array` parameters. The caller owns
  the memory (no hidden allocation/copy, no allocator coupling, trivial C
  ABI/export + FFI later, "nothing" is just `{nullptr, 0}`), and count is
  explicit at the boundary instead of implied by a container. Containers
  are fine as INTERNAL storage and as return values when convenient; the
  rule is about parameter lists. Batch/getter pairs use the same shape:
  `Things() const` + `ThingCount() const` (pointer + count), not a
  vector return. Historical span-taking RHI overloads exist
  (BulkCopyBuffer, the async readbacks) — don't propagate them to new
  code; when touching one, prefer adding a pointer+count overload.

## Strict resources (`--strict` / `TOURMALINE_STRICT=1`) + the RHI error contract

**Intention**: resource-pool exhaustion (bindless descriptor budget, image/view
pools) must never hide — but the RHI never decides policy. **The Vulkan RHI
returns error signals (empty handles) and the responsibility to handle them is
at call sites**: the default is graceful (skip/fall back — editing sessions
survive budget pressure), strict turns exhaustion into a LOUD abort at the
call site so silent skips cannot hide leaks or budget-size bugs. Asserts
INSIDE the RHI are for invariants only (programmer errors) — never for
operational conditions like pool exhaustion.

- `Editor --strict` (editor/src/main.cpp) and `TOURMALINE_STRICT=1` (any host,
  any build) gate `smvk::StrictResources()` (engine/include/Engine/smvk.h,
  implemented in engine/src/Vulkan/impl.cpp).
- **Call-site pattern** (the RHI has no abort paths):
  `if (handle.empty()) { smvk::OnResourceExhaustion("site"); return; }` —
  OnResourceExhaustion is a no-op unless strict, in which case it aborts with
  the diagnostic (`tourmaline_fatal.log` + stderr). Debug builds also keep the
  pools' own `assert`s — strict is the RELEASE-visible form.
- **Work is NOT yet done across the whole project**: the gate currently lives
  at the element churn sites in WorldRenderer (the demonstrated exhaustion
  points). The other creation sites still handle
  gracefully without the strict report. Spreading the gate is in-progress
  work; **using the pattern in new code is OK** (and expected at any new
  exhaustion-prone creation site).
## Linux compatibility

The tree builds and passes ctest on Linux (Ubuntu 24.04 + GCC 14 verified in
WSL; the same test suite as Windows is registered, passing on both). Conventions:

- `install_dependencies.sh` is the Linux installer: it carries the
  platform-specific pins (SDL3 3.4.16 SOURCE zip - the root CMakeLists
  builds it as a subproject when no system SDL3 exists; the DXC official
  linux package; Vulkan-Headers 1.4.357 - distro libvulkan-dev headers are
  too old; DirectX-Headers + DirectXMath + the dotnet-PAL sal.h for
  DirectXTex's non-Win32 path). Windows equivalents live in the .ps1.
- Toolchain: **GCC 14** (required for the verified build). GCC 13 ICEs in `gimplify` on the engine's
  array-NSDMI aggregates (see the NOTE at `struct ShaderProgram` in
  engine/include/Engine/smvk.h) - never write `x = ShaderProgram{}` or
  `ShaderProgram p{};` for that type; use the paren/default forms.
- The engine PCH is MSVC/Clang-only; GCC force-includes pch.h as a plain
  `-include` (GCC .gch does not carry #pragma-once state). Do not move
  TUs off pch.h's transitive includes without adding their own includes.
- `find_package(Ktx CONFIG)` - capital K (KTX 4.3+ ships KtxConfig.cmake;
  lowercase only matches on case-insensitive filesystems).
- `<ktxvulkan.h>` - KTX 4.3+ renamed the headers lowercase; on Windows the
  old spelling worked via case-insensitivity. Always use the lowercase
  includes.
- DXC staging + every runtime .so stage at the BUILD ROOT
  (`${TOURMALINE_BINARY_DIR}`), not the static lib's dir; Linux exes carry
  a `$ORIGIN` rpath (cmake/HostSetup.cmake + editor/CMakeLists.txt).
- Streamline/DLSS is Windows-only by design (engine/src/Renderer/dlss.cpp
  compiles stubs on Linux); never add Linux loads of the sl.* runtime.
- Runtime note: boots + loads + pipeline-compiles are verified on the
  mesa software rasterizer (lavapipe), but its compute-pipeline compiler
  crashes on the BDA-heavy engine shaders - the render loop needs a real
  GPU. Do not "fix" lavapipe crashes as port bugs without a GPU-verified
  repro.
