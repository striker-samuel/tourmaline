# GPU Scene removal: completion record

Status: **COMPLETE** (implementation landed on `remove-gpuscene`, merged to
`master`). This document was the acceptance plan for the single removal PR and
is now the completion record with evidence. Historical references to the GPU
Scene system that remain in the tree (docs/audit-prs/*, the TODO history
section, this record) are intentional.

## Branches
- Source baseline: `master`
- The original GPU Scene implementation is preserved, untouched, on
  `gpuscenework` in the original working repository (HEAD d3656306 at removal
  time).
- All removal work landed on `remove-gpuscene`; merged to `master` as one PR
  (#305).

## What was removed
- 66 GPU-scene-owned files (12 `engine/include/Engine/Renderer/gpu_scene*.h`
  + `gpu_scene_raster_*`, `engine/include/Engine/Scene/
  gpu_scene_memory_bindings.h`, `engine/src/Renderer/gpu_scene_upload.cpp`,
  27 shaders incl. `gpu_scene.hlsli` + `double_float.hlsli`, 9 test sources,
  2 tools scripts, 13 docs) - 115 files changed, +136/-11834 lines total.
- The embedded lane: ~1,850 lines in `WorldRenderer.cpp` (eligibility,
  raster-mode gate, compute loading, dispatch, temporal seed/Hi-Z/history
  chain, indirect Main bins), ~90 members in `WorldRenderer.h`,
  `AppendGpuScenePrepass` + ownership plumbing in `ForwardPlusRenderer.*`,
  the GfxFilter ownership carve-out (`SetGpuRasterOwnership`, seed skip,
  draw-push skip) in `GfxRenderer.*`, `gpuRasterOwnsNode`/
  `gpuSceneDepthPrimed` in `FrameContext.h`, the Scene mutation journal, the
  World cluster-id map, the RHI `CreateGpuSceneBuffer` VMA 64-KiB row-page
  pool lane, `TOURMALINE_GPU_SCENE_RASTER/TEMPORAL/VALIDATE` env latches,
  the `.sentinel` shader variant row, `SMVK_PROFILE_GPU_SCENE`.
- GfxFilter is restored as the sole main-world filtering/raster path:
  `gfxFilterOn = m_passes.gfxFilter && m_gfxFilterAvailable`, the Main
  seed/remainder run unconditionally again, the Forward+ prepass is always
  the legacy CPU walk, and the occluder depth fill + HZB build are
  unconditional (frozen-culling-view remains the only skip).
- Kept intact: GfxFilter (`gfx_filter.*`, gfxfilter/gfxbatchcull/hzb
  shaders, `test_gfxfilter.cpp`), GPU placement, mesh particles, static SDF
  (its `StaticSdfGpuScene` class is SDF's own concept, name-only overlap),
  the debug-draw channel decoupling (PR #298), the KD diagnostic family.
- `World::gpuSceneTopologyRevision` renamed `sceneTopologyRevision` (static
  SDF republish key; the snapshot key `topology_revision` is unchanged).

## Required validation (evidence)

- [x] `gpuscenework` still points to the original master snapshot and its GPU
  Scene work remains intact (original repository checked out on it at
  d3656306; the removal was developed in a separate clone).
- [x] No remaining production GPU Scene render passes, shader modules,
  pipelines, buffer ownership, env flags, shader build entries, or dead
  symbols. Whole-tree grep leaves only: the `StaticSdfGpuScene` class (SDF's
  own, kept), a vendored optick enum (external/, untouched), and the
  intentional historical references named above. `engine-shaders.pak`
  dropped 218 -> 192 blobs (exactly the 26 `gpu_scene_*.spv`).
- [x] CMake configures from a clean build directory. Command (from the
  repository root, after deleting `out/build/x64-Release`):
  `cmd /c "set EMBREE_PREBUILT_DIR=F:/tools/embree-4.4.1&& build.bat"`.
  Environment: Windows, Ninja + Release, MSVC 19.52.37025.0 (VS 2026
  Insiders), Vulkan SDK 1.4.357.0, prebuilt Embree 4.4.1 at
  `F:/tools/embree-4.4.1`, vendored SDL3 3.4.16 devel package.
- [x] Full engine and app build succeeds, including shader compilation and
  packaging: 1156/1156 targets (engine, App, Benchmark, tools, all test
  targets); engine-shaders.pak packed from the declared list and verified
  (192 blobs read back OK).
- [x] `GfxFilterTests` builds and passes; full
  `ctest --test-dir out/build/x64-Release --output-on-failure`:
  **100% tests passed, 109/109** (twice: incremental and after the clean
  reconfigure), including architecture_tests (R1-R9 boundary alarm),
  rendergraph_contract_tests, scene_memory_bindings_tests,
  shader_variant_manifest_tests, runconfig_tests and project_e2e_tests.
- [x] Runtime smoke test on a Vulkan-capable GPU renders a representative
  scene via GfxFilter with no missing shader or validation errors:
  NVIDIA GeForce RTX 5070 Ti, driver 617.42, Windows; `sponza_10.tapp`
  headless bench exits 0; log shows `[GfxFilter] view: 65 draws, 2710
  batches, 142091 tris processed` + `dispatch ok` (VK_EXT_mesh_shader
  task+mesh path enabled).
- [x] Visual comparison: `tools/tourcapture.py` on the canonical
  sponza-atrium camera (8.82,5.38,-0.60 / -0.988,-0.018,0.152, AA off, auto
  exposure off, frame 48 of 120) against the preserved pre-removal GPU-Scene
  binaries: **decoded-pixel RGB SHA-256 identical**
  (8cb9f24a2f8eafa2c0fa1cf773007077b9ec003b7aa65a3e572e300bde10150b);
  `pixel_compare.py --stride 1`: **0 / 2,073,600 differing pixels**, twice
  (incremental build and clean rebuild). Opaque/masked materials, alpha
  cutout, depth/Hi-Z occlusion, G-buffer/SSR, shadows, LODs and the
  fallback legs are all exercised by this scene and byte-identical.
- [x] No duplicate draws and no missing meshes across GfxFilter
  availability/disabled states: identical filter stats on both builds
  (65 draws / 2710 batches / 142091 tris) and the exactly-once Main
  contract is pinned by gfxfilter_tests + rendergraph_contract_tests.
- [x] Core frame-time comparison, sponza-atrium scenario, 5-run medians of
  per-run average frame times via `tools/perf_harness.py`:
  baseline (GPU-Scene build) 6.3727 ms, candidate 6.3501 ms,
  **-0.35% (no regression; 5% threshold)**. The tiny win matches the
  removed dead per-frame GPU-scene sync/upload CPU work.
- [x] Diff reviewed for accidental removals of shared functionality: all
  136 inserted lines are the intended restorations (GfxFilter condition
  simplifications, the `sceneTopologyRevision` rename, comment rewords, the
  `common.hlsli` exclusion-list token drop).

## Honest boundaries / limitations
- On ALL shipped demo content (sponza_10, sponza_grid - both GI scenes) the
  GPU-scene raster gate refused the cutover before the removal (KD
  diagnostics on the pre-removal binary: grid scene 1751/1751 eligible but
  `gi=2` refusal), so both builds rendered through GfxFilter and the pixel
  identity above proves output-neutrality of the removal on this content -
  it is NOT a GPU-scene-raster vs GfxFilter image comparison. No
  GPU-scene-admitted content was available to capture; that lane was dead
  on shipped content, which is the removal's motivation.
- Snapshot content: world snapshots lose `next_gpu_scene_cluster_id` and
  the `*_owner_cluster_id` family of rows (their only stable numeric
  identity was the removed GPU-scene cluster-id map; raw pointers are
  excluded by the snapshot's own rules). The `topology_revision` key is
  preserved under the renamed member.
- `gpu_struct_layout_tests` ctest registration was removed with its only
  checked pairs (GpuSceneMesh, double_float); `tools/check_gpu_struct_layout.py`
  remains functional for explicit `--cpp/--hlsl` inputs and its unit test
  still runs.
- All runtime evidence is from one machine (RTX 5070 Ti, driver 617.42,
  Windows); the perf harness's exclusivity gates were satisfied in a
  quiet-GPU window (desktop GPU apps can abort the preflight - retry).

## PR completion policy
The checklist above is backed by recorded commands and results, not
self-assertion. Build/test logs live under `out/build/x64-Release`,
captures under `out/captures/`, perf results under `out/perf/` in the
development clone (not committed).
