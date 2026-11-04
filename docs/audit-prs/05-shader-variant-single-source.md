# Single-source shader variants for build and hot reload

> **STATUS: IN PROGRESS.** Implementation committed: one manifest now feeds CMake and generated C++ runtime variants. Native CI and shader build verification remain. This branch is not merge-ready until all acceptance tests and remaining scope are complete.

**Finding:** L-07  
**Priority:** P0

## Problem and intended change
Replace separately-maintained CMake and C++ shader variant tables with a generated declarative variant manifest shared by both.

## Source files to inspect / potentially modify
- `cmake/CompileShaders.cmake`
- `engine/CMakeLists.txt`
- `engine/src/Utils/shader_hot_reload.cpp`
- `engine/src/Utils/shader_library.cpp`
- `tests/test_shader_hot_reload.cpp`

## Acceptance criteria
CI proves build and hot reload variants identical; include dependencies, missing variants and invalid compiler results preserve last-known-good outputs.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
