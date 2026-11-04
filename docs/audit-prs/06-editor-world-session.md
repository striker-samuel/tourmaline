# Isolate per-world editor session state

> **STATUS: IMPLEMENTATION NOT STARTED.** This is a draft PR scaffold containing an audit-derived scope specification only. Do not merge until actual implementation commits, tests, and review are complete.

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
