# Decompose WorldRenderer into cohesive collaborators

> **STATUS: IMPLEMENTATION NOT STARTED.** This is a draft PR scaffold containing an audit-derived scope specification only. Do not merge until actual implementation commits, tests, and review are complete.

**Finding:** L-01  
**Priority:** P1

## Problem and intended change
Extract narrow frame-stage orchestration/services from the oversized renderer without replacing render paths or introducing general GPU memory ownership.

## Source files to inspect / potentially modify
- `engine/src/Renderer/WorldRenderer.cpp`
- `engine/include/Engine/Renderer/WorldRenderer.h`
- `engine/src/Rendergraph/rendergraph.cpp`
- `tests/test_rendergraph_contract.cpp`

## Acceptance criteria
Render output and execution order remain unchanged; unit tests pin initialization, per-pass state, shutdown and no regression in renderer benchmarks.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
