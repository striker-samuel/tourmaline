# Isolate per-world editor session state

> **STATUS: IMPLEMENTATION COMMITTED — INTEGRATOR TODOs REMAIN.** Safe per-world selection restoration and failed prefab-open recovery are implemented. Larger session ownership extraction remains with the integrator. Do not merge until actual implementation commits, tests, and review are complete.

**Finding:** L-08  
**Priority:** P1

## Problem and intended change
Extract the minimum session-owned document, selection, undo/redo, camera and tool context; preserve shared device and caller-owned GPU lifetime.

## Source files to inspect / potentially modify
- `editor/src/editor_app.h`
- `editor/src/editor_prefab_multiworld.cpp`
- `editor/src/editor_world.cpp`
- `editor/src/editor_viewports.cpp`
- `editor/src/editor_commands.cpp`
- `tests/test_prefab_live_sync.cpp`

## Acceptance criteria
Parent and prefab edit sessions stay isolated through save/revert/close/resize; secondary viewport GPU smoke tests are included.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.

## Implemented
- Added GPU-free `editor_world_session.h` selection restore policy: validates the destination hierarchy, deduplicates selections and handles missing/stale primary nodes.
- Applied this policy on normal prefab-return **and failed prefab-open recovery**. Previously failed opens restored the scene file/path/dirty flag but did not restore parent cameras or selection.
- Recovery failure now retains the parent snapshot on disk with a diagnostic instead of deleting it after a failed reload.
- Added headless regression cases in `test_prefab_live_sync.cpp` for duplicate/stale selections, missing world and a single primary.

## Integrator TODO
- [ ] Extract full document, command history/undo-redo, camera and tool state into independently owned parent/prefab editor sessions (this commit addresses safe transitions, not complete isolation).
- [ ] Run native Editor + PrefabLiveSyncTests; manual save/revert/close/reopen/resize smoke tests and GPU secondary-viewport checks.
- [ ] Validate history and scene dirty-state transitions, especially failed return saves and prefab load recovery.
- [ ] Confirm caller-owned GPU lifetime, rendering parity, and before/after benchmarks.

**Validation:** Source-level checks only; native/GPU tests were not executed here. Implementation is committed and integration follow-ups are assigned to the integrator.
