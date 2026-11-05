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
Legacy float-facing runtime arrays remain available; authoritative double transforms flow through hierarchy recalculation and GPU-scene publication. New scene files use a versioned transform encoding with 16 binary64 scalars per local/global matrix, preserving large fractional world coordinates. Legacy unversioned scene files remain readable with their float32 matrices, promoted to doubles in memory. Older binaries cannot read the new versioned files; re-export in the old format is not provided. Integrator owns native compilation, GPU parity and performance baselines.

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

## Versioned scene binary layout (Audit 03)

- New file prefix: `uint32 magic = 0x54444D53`, `uint32 version = 1`,
  `uint32 liveNodeCount`. The magic is outside the valid legacy node-count
  range, so unversioned files are detected without ambiguity.
- After the prefix: `liveNodeCount` local affine matrices, then the same
  number of global matrices; each matrix is 16 contiguous IEEE754 binary64
  scalars in GLM column-major order, written individually to avoid GLM
  structure packing assumptions. Hierarchy and all remaining scene sections
  retain their existing native binary representation and order.
- Files with no magic use the original `uint32 nodeCount`, float32 local/global
  matrices, hierarchy and optional sections. Unsupported v1 versions and
  invalid/truncated fixed headers are rejected; no silent float32 fallback.
- Storage is not an independent endian-neutral interchange format. The
  existing scene tail also contains native records. The same platform/
  architecture expectations therefore continue to apply.
- Regression coverage: large positive/negative fractional nested scene
  save/load, unknown-version rejection, and legacy float32 fixture loading.
  Test execution and end-to-end editor/cooker consumer checks remain assigned
  to the integrator.
