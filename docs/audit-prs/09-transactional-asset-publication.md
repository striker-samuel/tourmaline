# Transactional identity and package publication contracts

> **STATUS: IMPLEMENTATION NOT STARTED.** This is a draft PR scaffold containing an audit-derived scope specification only. Do not merge until actual implementation commits, tests, and review are complete.

**Finding:** L-12  
**Priority:** P0

## Problem and intended change
Specify source-to-artifact-to-catalog-to-pak lineage and atomic publication boundary; enforce recoverable partial failures and hash/provenance checking.

## Source files to inspect / potentially modify
- `tools/project_pipeline/src/build_pipeline.cpp`
- `tools/artifact_cache/src/assetdb.cpp`
- `engine/src/Asset/pak.cpp`
- `engine/src/Asset/appmanifest.cpp`
- `engine/src/Vfs/vfs.cpp`
- `tests/test_project_e2e.cpp`

## Acceptance criteria
Failed cook, partial publish, interrupted update and stale provider cache cannot expose mixed-generation content as valid.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
