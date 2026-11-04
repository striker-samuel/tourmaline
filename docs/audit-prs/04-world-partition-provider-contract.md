# WorldPartition provider integration contracts

> **STATUS: IMPLEMENTATION NOT STARTED.** This is a draft PR scaffold containing an audit-derived scope specification only. Do not merge until actual implementation commits, tests, and review are complete.

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
