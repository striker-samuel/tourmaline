# GPU Scene removal: implementation and validation plan

Status: **planning only**. This document establishes acceptance criteria for the single removal PR. No rendering implementation is changed by this document commit.

## Branches
- Source baseline: `master`
- Preserve the existing GPU Scene implementation on `gpuscenework` (do not change this branch).
- Implement all removal work on `remove-gpuscene`; open one PR to `master`.
- Do not merge until every validation criterion below passes.

## Implementation instructions
1. Inventory GPU Scene symbols, build declarations, shader outputs, configuration flags, and rendering branches. In particular inspect `engine/src/Renderer/WorldRenderer.cpp`, `engine/include/Engine/Renderer/WorldRenderer.h`, scene/world state, `engine/CMakeLists.txt`, root `CMakeLists.txt`, and `tests/CMakeLists.txt`.
2. Restore **GfxFilter as the sole main-world filtering/raster ownership path**. Remove GPU Scene eligibility, canonical raster ownership, work generation, indirect commands, temporal/Hi-Z path, shader/pipeline setup, GPU database sync/upload/readback and validation path. Restore unconditional GfxFilter and legacy fallback behavior where GPU Scene had overridden or suppressed it. Keep unrelated GPU systems such as GPU placement, mesh particles and static SDF reflections unless they are genuinely dependent on the removed implementation.
3. Remove obsolete GPU Scene C++ headers/sources, HLSL shaders, shader compilation/packaging entries, dedicated run-config environment variables and UI toggles, GPU Scene tests and scripts, and retired docs; update remaining documentation. Remove references rather than leaving dead code or always-false flags.
4. Keep `engine/shaders/gfxfilter.cs.hlsl`, `gfxfilter.hlsli`, `gfxfilter.mesh.hlsl`, `gfxfiltered.vert.hlsl`, `gfxfilteredmotion.vert.hlsl`, `tests/test_gfxfilter.cpp`, and their required pipeline/build integration. Preserve depth, G-buffer, shadows, material cutouts, MSAA, motion vectors, and fallback rendering.
5. Refactor carefully: search for transitive references, delete now-unused structs and members, fix include dependencies and initialization/teardown, and avoid accidentally deleting shared primitives.

## Required validation (check off with evidence)
- [ ] `gpuscenework` still points to the original master snapshot and its GPU Scene work remains intact.
- [ ] No remaining production GPU Scene render passes, shader modules, pipelines, buffer ownership, env flags, shader build entries, or dead symbols. Any remaining historical references are explicitly justified.
- [ ] CMake configures from a clean build directory on a supported toolchain. Record the exact configure command and environment.
- [ ] Full engine and app build succeeds, including shader compilation/packaging. Record the exact build command.
- [ ] `GfxFilterTests` builds and `ctest --output-on-failure` passes for `gfxfilter_tests`; run other relevant renderer/headless tests. Record commands and outcomes.
- [ ] Runtime smoke test on a Vulkan-capable GPU renders a representative scene via GfxFilter with no missing shader or validation errors; capture log and hardware/driver details.
- [ ] Visual comparison checks opaque and masked materials, alpha cutout, depth/Hi-Z occlusion, G-buffer/reflections, shadows, MSAA/A2C, motion/TAA, LODs, debug views, wireframe and fallback rendering.
- [ ] Confirm no duplicate draws and no missing meshes when switching GfxFilter availability/disabled states; check resize/reload/scene switching and large-world cases.
- [ ] Compare core frame-time, draw/dispatch counts and relevant memory footprint against baseline; explain changes or regressions.
- [ ] Review diff for accidental removals of shared functionality; PR contains only intended cleanup, tests and docs.

## PR completion policy
Keep the PR in draft while the implementation or validation is incomplete. Do not treat the checklist itself as passing evidence. Add the precise build/test commands, results and any limitations to the PR before requesting review. Do not merge unless all required checks are satisfied.
