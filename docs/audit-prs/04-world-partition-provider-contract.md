# WorldPartition provider integration contracts

 > **STATUS: IMPLEMENTATION COMMITTED — INTEGRATOR TODOs REMAIN.** Source and tests are committed; integration, runtime validation and outstanding contracts are assigned to the integrator.

**Finding:** L-05  
**Priority:** P1

## Problem and intended change
Inventory actual streaming providers and introduce explicit interest/request/commit/cancel expectations while preserving provider-owned memory and eviction.

## Source files to inspect / potentially modify
- `engine/include/Engine/WorldPartition/WorldPartition.h`
- `engine/src/WorldPartition/WorldPartition.cpp`
- `engine/include/Engine/WorldFields/WorldFieldProvider.h`
- `engine/src/Renderer/WorldRenderer.cpp`
- `tests/test_world_partition.cpp`

## Acceptance criteria
Teleport, stale request, frame commit and unload paths are deterministic; no general streaming or GPU memory manager is introduced.

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
- Added runtime duplicate/null provider rejection in `WorldPartition::AddProvider` (also effective in Release builds).
- Empty/null interest sets now clear previous interest state without null pointer arithmetic, supporting teleport/unload cancellation.
- Added headless tests for duplicate registration, cancellation, and commit/retire progression after cancellation.
- **Not validated:** native build/CTest, GPU workload, provider async generation cancellation, frame epochs or stale completion filtering. Providers still own memory and eviction.

**Integrator TODO:** Complete provider lifecycle integration and acceptance validation; implementation work is committed.

## Integrator handoff
The PR is ready for review with implementation committed. The outstanding acceptance items above are TODOs for the integrator, not a claim that tests have passed or that integration is complete.
