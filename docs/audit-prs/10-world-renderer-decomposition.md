# Decompose WorldRenderer into cohesive collaborators

> **STATUS: PARTIAL IMPLEMENTATION.** The CPU-only frame-pass configuration has been extracted and regression checks added. Frame-stage orchestration decomposition and native/GPU acceptance testing are **not complete**. Keep PR #191 as a draft; do not merge yet.

**Finding:** L-01  
**Priority:** P1

## Problem and intended change
Extract narrow frame-stage orchestration/services from the oversized renderer without replacing render paths or introducing general GPU memory ownership.

## Implemented in this branch
- [x] Extract `WorldRendererConfig` and its `All`, `Minimal`, and `FilterOnly` policies into `engine/include/Engine/Renderer/WorldRendererConfig.h`.
- [x] Preserve public API and include the new policy header from `WorldRenderer.h`; no Vulkan execution or resource ownership changes.
- [x] Add CPU contract checks in `tests/test_rendergraph_contract.cpp` for all/minimal/filtered presets and independent configurations.
- [x] Inspect the existing implementation, header, and rendergraph contract tests.

## Remaining acceptance gates
- [ ] Trace frame-stage call graph, extract appropriately narrow orchestration collaborators, and preserve order/feature fallback.
- [ ] Test initialization, shutdown, error paths and integrations; the added contract assertions only cover policy.
- [ ] Run CMake/native build and RendergraphContractTests on supported compilers.
- [ ] Run GPU smoke, validation-layer and benchmark before/after tests.
- [ ] Record verification evidence and confirm backwards compatibility and caller-owned GPU lifetime.

## Scope boundary
Do not introduce a GPU memory manager or automatic lifetime ownership; the caller remains responsible for GPU memory. Do not enable experimental rendering paths. No renderer benchmarks or native builds were executed through the connector.
