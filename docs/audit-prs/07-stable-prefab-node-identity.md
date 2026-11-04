# Stable identities for prefab source-node synchronization

> **STATUS: IN PROGRESS.** Partial implementation committed: UUID-first preview matching and authoring, fail-closed duplicates, legacy fallback. Identity migration and editor verification remain. This branch is not merge-ready until all acceptance tests and remaining scope are complete.

**Finding:** L-09  
**Priority:** P0

## Problem and intended change
Map prefab source nodes to parent occurrences using persistent structural source IDs instead of namespace+display-name matching; retain override precedence.

## Source files to inspect / potentially modify
- `editor/src/editor_prefab_sync.h`
- `editor/src/editor_prefab_multiworld.cpp`
- `engine/include/Engine/Asset/tour.h`
- `engine/include/Engine/Scene/scene_tables.h`
- `tests/test_prefab_live_sync.cpp`

## Acceptance criteria
Duplicate names, source rename, reimport and undo/revert cannot update the wrong occurrence; existing authored scenes have an explicit compatible migration strategy.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
