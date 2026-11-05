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
- [x] Preserve populated double entries during partial legacy-array growth and repair; test mixed double/float parent-child hierarchies.
- [x] Apply merged-scene root offsets to double-local transforms and synchronize the float shadow.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Compatibility
Legacy float arrays and binary fields retain their format; doubles are preserved at runtime with the new dmat4 setter and in the GPU scene bridge. Legacy disk serialization remains float32 and is **not** lossless for large double coordinates. A format-version migration is still required for precise disk round-trips. Integrator owns native compilation, GPU parity and performance baselines.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.

## Additional implementation notes

- `PrepareDoubleTransforms` and `SetLocalTransform` now extend partially populated
  double/shadow arrays without resetting existing double-precision transforms.
- `mergeScenes` applies legacy root transform offsets in double precision and
  synchronizes the float shadow so the next recalc cannot import a rounded root.
- `test_scene_tables.cpp` includes an array-repair regression at absolute
  coordinates near 1e12 with fractional translations and a parent-child chain.
- Native build, CTest, graphics parity, and performance measurements were **not
  performed** by this author; the integrator must validate those gates.
- This change does **not** make the legacy float32 scene file format lossless.
  Retain that compatibility limitation until an explicitly versioned format
  migration is designed and verified.
