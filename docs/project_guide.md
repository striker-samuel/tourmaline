# VulkanToy — Project Guide for Future Agents

Source of truth alongside `AGENTS.md` (build/run), `docs/editor_tour_and_context.md`
(tour v1 spec), `ALLOCATION_ANALYSIS.md` (allocator migration). `README.md` is
empty; `notes/` is gitignored.

## 1. What this is

C++20 Vulkan 1.3 renderer + toy engine (`ENGINE_BACKEND=Vulkan` only).

```
Loose glTF/KTX2 -> gltf_import -> PakCooker(+GiBaker/cubemap/lightmap) -> *.pak
  -> App.exe pak-viewer (app/) / Editor.exe DG (editor/) -> engine static lib
  -> RHI ResourceManager + RenderGraph -> DXC SPIR-V -> swapchain
```

Targets: `engine`, `App`, `Editor`, `PakCooker/GiBaker/CubemapBaker/gltf_import/
artifact_cache`, `01_triangle/02_blur`, `EngineScene_shaders/EngineRes_shaders`
(ALL), 27 plain-exe tests (no gtest) via ctest.

God objects are INTENTIONAL (owner decision): `WorldRenderer.cpp` (~14k lines)
and `Vulkan/impl.cpp` (~12k lines) stay whole. Do not propose splitting them
again. Extend via the seams below instead.

## 2. Build / run / test (Ninja only)

```powershell
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build out/build/x64-Release --target App
ctest --test-dir out/build/x64-Release --output-on-failure
```

- Never the VS generator (configure-time guard enforces this); mismatch means
  deleting `out/build/<config>` and reconfiguring. `cl.exe` needs `vcvarsall`
  from a plain shell (see `AGENTS.md`, `build_app.bat`).
- Shaders: `engine/shaders/*.vert|frag|cs.hlsl` -> `mainVS/mainPS/mainCS` via
  `cmake/CompileShaders.cmake` (DXC `-spirv -fspv-target-env=vulkan1.3
  -fvk-use-dx-layout`) -> `out/build/<cfg>/content/shaders/*.spv`, synced next
  to `App.exe`. Rebuild `App` on edit. Debug is `-Od` except
  `HLSL_FORCE_O3_FILES` (driver AV pin; retry via
  `python tools/shader_od_retry.py` + `-DVULKANTOY_SHADER_OD_RETRY=ON`).
- Run CWD = `out/build/<cfg>`; `--bench N` = 10 warmup + N measured then exit.
  `VULKANTOY_CAPTURE` must land INSIDE the bench budget.
- Headless matrix: `VULKANTOY_SSR/AA/AUTO_EXPOSURE/CAM_POS/CAM_FWD/CAPTURE`,
  `AO=off|ssao|sao`, `EVSM`, `VOLUMETRIC[_STEPS][_CVOL]`, `MESH=0`,
  `GFX_GPU_CULL=0`, `ASYNC_COMPUTE=0`, `NO_VALIDATION=1` — all declared in
  `engine/include/Engine/App/RunConfig.h`.
- Redirect `*.log` output to `out/logs/` (root is for sources, not logs).

## 3. Systems and seams (extend here, not in the god objects)

| System | Key files | Seams / contracts |
|---|---|---|
| App shell | `engine/App/app.h`, `app/src/Host.h+PakApp*.cpp` | `WorldRendererConfig::All/Minimal/FilterOnly` + `--with/--without` CSV; `SetWorldSource` injects content (engine never knows paks) |
| Frame renderer | `engine/Renderer/WorldRenderer.h` | `Configure(passes, run)` (parse-once `RunConfig`); `IFrameRenderer::AppendPasses(AppFrameContext)` |
| Push constants | `engine/Renderer/Renderer.h`, `engine/shaders/push_constants.hlsli`, `tools/push_constants_idl/` | 256B, shared 0..192, scratch 192..256; `codegen.py --check` + `PushConstantLayoutTests` |
| Run config | `engine/App/RunConfig.h+.cpp` | `Parse()` once in `main`, `Log()`, `WarnUnknown()`; new knobs go here first |
| Rendergraph | `engine/Rendergraph/*.h`, impl in `engine/src/Vulkan/impl.cpp` | `Add*Hints` barrier discipline; `RendergraphContractTests` |
| GPU scene | `engine/App/world.h+cluster.h`, `engine/Scene/scene.h` | `ResolveAndFixup_clusters` rebuilds `drawItems/flagLists/lodChains` |
| Cook/pak | `engine/Asset/pak.h`, `tools/pak_cooker/main.cpp` | `SMP5/KPM5` magic-bump only, no version field; `PakBackend` serves textures |
| Tour | `engine/Asset/tour.h+.cpp`, `editor/src/editor_commands.cpp` | `CmdToTour`/`TourToCmd`/`ApplyTourToCluster`/`ReplayTourDoc`; `TestTourFuzz` |
| Memory | `engine/Memory/memory.h+linear_vector.h` | `FrameVector` = FrameScratch (reset/frame); `UploadInFlightVector` = monotonic (never reset while live); `ProcessVector` = process (ThreadPool ring) |
| GPU-driven filter | `engine/Renderer/gfx_filter.h` | DEFAULT ON (`m_useGfxMesh/m_useGfxFilter`, `GpuCullEnabled`); kills: `VULKANTOY_MESH=0`, `VULKANTOY_GFX_GPU_CULL=0`; `MeshDrawEligible` truth table in contract tests |
| Goldens | `tools/golden_capture.py`, `tools/golden_hashes.txt` | `--update` records, CI fails on mismatch (per-GPU baselines) |

Data flow per frame: `DrawFrame` fills `AppFrameContext` -> `GfxRenderer`
(seed/HZB/filter) -> `ForwardPlus` (prepass+cull) -> shadow atlases
(CSM/spot/EVSM/deferred) -> `Main` (mesh leg first, indexed skips eligible
ranges: exactly-once) -> motion/DoF/SSR/volumetric/exposure/bloom/tonemap/
AA/UI -> swapchain.

## 4. Proofread backlog (highest value first)

1. `pak.cpp`: TOC/blob `u64` overflow checks, `Payload` entry-provenance,
   `ThreadPool(0)` hang (pool ctor now guards; pak sites still use
   `min(count, hc)` — wrap with `max(1u,…)` when touched).
2. `impl.cpp` barrier collection: `continue`-on-missing skips (assert+log
   instead); `%2` SDSM slots vs 3 flights (verify/convert to flight count).
3. Bindless: exhaustion must fail closed (`UINT32_MAX`, not assert-only);
   destroy path is being hardened upstream (`NullBindlessSlotWrites`) —
   coordinate, don't duplicate.
4. `AppFrameContext::gfxLodFilterable` (`std::function`, nullable): null-guard
   at call sites; don't copy per frame.
5. `RenderGraph::NewFrame` pool growth: free/reuse transients, don't
   `AddImage` per frame without free.

## 5. Conventions for agents

- Push structs: C++ and HLSL mirrors stay layout-identical — run
  `codegen.py --check`, keep `static_assert`s, never add sub-192 fields
  (FS-only extras go in `FrameUniforms::frameHot`).
- New `VULKANTOY_*`: declare in `RunConfig.h` + parse in `RunConfig.cpp` +
  extend `WarnUnknown` + cover in `test_runconfig.cpp`. Never scatter `getenv`.
- New scratch vectors: `FrameVector` (dies in scope) vs
  `UploadInFlightVector` (spans frames) — wrong choice is UAF or leak.
- New shaders: include `push_constants.hlsli` directly when possible
  (fine-grained deps); `common.hlsli` umbrella still works.
- New tests: plain-exe + `add_test` + `vulkantoy_stage_sdl3`, hand-rolled
  `Check()` runner, `checks=/failures=` summary, exit nonzero on failure.
- Don't use F12; don't bump format versions / add compat shims (magic-bump
  to break old files is OK).
- Heavy explanatory comments are the repo norm — keep reasoning auditable.
