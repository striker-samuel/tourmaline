# Authoritative double-precision world transforms

> **STATUS: IMPLEMENTED FOR INTEGRATOR VALIDATION.** Native compilation, GPU parity and benchmark checks remain with the integrator.

**Finding:** L-04  
**Priority:** P0

## Problem and intended change
Retain double-precision absolute translation through scene hierarchy and scene-to-renderer publication; derive float matrices only after camera-origin subtraction.

## Source files to inspect / potentially modify
- `engine/include/Engine/Scene/scene.h`
- `engine/src/Scene/Scene.cpp`
- `engine/include/Engine/Core/double_float.h`
- `engine/src/App/world.cpp`
- `engine/include/Engine/Renderer/gpu_scene_world.h`
- `tests/test_doublefloat.cpp`

## Acceptance criteria
Large positive and negative coordinates and nested parent chains round-trip with stable camera-relative motion; preserve format compatibility or version migration explicitly.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [x] Preserve double local/global transforms and publish their double values to the GPU scene.
- [x] Add nested signed, invalid-handle, NaN and legacy float writer assertions to test_scene_tables.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Compatibility
Legacy float arrays and binary fields retain their format; doubles are preserved at runtime with the new dmat4 setter and in the GPU scene bridge. Legacy disk serialization remains float32 and is **not** lossless for large double coordinates. A format-version migration is still required for precise disk round-trips. Integrator owns native compilation, GPU parity and performance baselines.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
