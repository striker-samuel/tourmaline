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

MSVC environment: `cl.exe` needs `vcvarsall` when building from a plain shell:

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul && cmake --build out/build/x64-Release --target App"
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
- `TOURMALINE_VOLUMETRIC=0|1` forces the raymarched fog on/off headless
  (scene fog settings lose); `TOURMALINE_VOLUMETRIC_STEPS=<4..128>` pins the
  march steps for heavy fog benches; `TOURMALINE_VOLUMETRIC_CVOL=0` is the
  fog-GI quality bypass (octree/sky only, no convex-volume walk per GI
  refresh) — pairs with `TOURMALINE_VOLUMETRIC` for pure-octree fog cost A/B
- `TOURMALINE_PROBE_PREMIX=0|1` pins the Remember Me probe pre-mix
  (docs/probe_premix_plan.md; default on): `1` = the per-frame GPU weights
  + mix passes (a blended cubemap + a GPU-owned selection; the material
  does ONE fetch + dominant-box parallax), `0` = the per-pixel walk
  fallback (≤8 fetches + per-pixel box parallax). `0.00%` differing pixels
  is the A/B contract between the two
- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off - uncapped immediate present; an explicit `--present <mode>`
  wins)

## Tests

Tests are plain executables (no gtest), built by the normal target build and
run via ctest from the build directory:

```powershell
ctest --test-dir out/build/x64-Release --output-on-failure
```

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
  `kGiBakeTexelMaxDim` 512); the GI fields (.sggi octree + cvols) bake
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
- Dont use F12 for any binding since it can trigger debugbreak
- Dont ever bump format versions or add conversions to keep old data working. Bumping magic to break old files is ok. I have no important assets to keep

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