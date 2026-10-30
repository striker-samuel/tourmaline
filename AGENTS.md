# AGENTS.md

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

## Submodules

Unmodified upstream deps are pinned git submodules under `external/`
(nanosvg, volk, SPIRV-Reflect, glm, meshoptimizer, DirectXTex, json). After
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
  contact pass stays LIVE under this view (it samples the real march)
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
  chroma — verified on Sponza). Sponza GI flow: `tools/cook/cook_sponza.bat`
  runs Cook → GiBaker (the convex volumes + the global octree from
  `F:/content/models/sponza/Sponza.tour`) → Pak
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
WSL; full test parity with Windows). Conventions:

- `install_dependencies.sh` is the Linux installer: it carries the
  platform-specific pins (SDL3 3.4.16 SOURCE zip - the root CMakeLists
  builds it as a subproject when no system SDL3 exists; the DXC official
  linux package; Vulkan-Headers 1.4.357 - distro libvulkan-dev headers are
  too old; DirectX-Headers + DirectXMath + the dotnet-PAL sal.h for
  DirectXTex's non-Win32 path). Windows equivalents live in the .ps1.
- Toolchain: **GCC 14+**. GCC 13 ICEs in `gimplify` on the engine's
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
