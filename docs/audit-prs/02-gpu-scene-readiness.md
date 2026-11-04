# Explicit GPU scene record readiness and legacy fallback

> **STATUS: PARTIAL IMPLEMENTATION.** A CPU-side readiness gate has been added and tested in source; the live draw path remains unchanged. Do not merge until actual implementation commits, tests, and review are complete.

**Finding:** L-03  
**Priority:** P0

## Problem and intended change
Define readiness and generation/epoch checks for mesh/LOD/submesh/instance/leaf records; prevent provisional records from reaching draw consumers; retain legacy path.

## Source files to inspect / potentially modify
- `engine/include/Engine/Renderer/gpu_scene.h`
- `engine/include/Engine/Renderer/gpu_scene_world.h`
- `engine/include/Engine/Renderer/gpu_scene_candidates.h`
- `engine/src/Renderer/WorldRenderer.cpp`
- `engine/shaders/gpu_scene.hlsli`
- `tests/test_gpu_scene_validation.cpp`

## Acceptance criteria
Invalid references, stale generations, nonresident geometry or missing bounds cannot emit draws; compare candidate selection against legacy renderer.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.

## Implementation evidence (partial)
- Added `InstanceDrawReady` predicate to `gpu_scene_world.h`: rejects invalid instance/mesh generations, missing mesh or LOD bounds, inconsistent residency, missing filterable submeshes, and nonresident geometry.
- Added CPU tests for provisional, missing-bounds, ready, stale-mesh-generation and evicted cases in `test_gpu_scene_world.cpp`.
- Existing legacy renderer and GPU memory ownership are unchanged. The predicate is **not yet connected to draw emission**, and it does **not** prove per-flight publication epochs or submesh/LOD generations.
- Native CMake/CTest and GPU draw parity tests were **not run** in this environment.

**Keep the PR draft. Further consumer integration and validation are required before this P0 finding is resolved.**
