# Scene mutation journaling and topology revisions

> **STATUS: IN PROGRESS.** Partial implementation committed: generation-checked transform setter, animation migration and dynamic-root journal fix. Complete caller migration and integration tests remain. This branch is not merge-ready until all acceptance tests and remaining scope are complete.

**Finding:** L-02  
**Priority:** P0

## Problem and intended change
Centralize supported scene mutations, guarantee GPU-scene dirty journal and topology revision updates, and prohibit bypass through supported mutation interfaces.

## Source files to inspect / potentially modify
- `engine/include/Engine/Scene/scene.h`
- `engine/src/Scene/Scene.cpp`
- `engine/src/Scene/SceneTables.cpp`
- `engine/include/Engine/Renderer/gpu_scene_world.h`
- `editor/src/editor_world.cpp`
- `tests/test_scene_tables.cpp`

## Acceptance criteria
Reparent, remove/reuse, transform, visibility and binding mutations cannot produce stale GPU records; add randomized and deterministic CPU/GPU snapshot parity tests.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
