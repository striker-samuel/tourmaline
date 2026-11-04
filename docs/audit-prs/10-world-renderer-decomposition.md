# Decompose WorldRenderer into cohesive collaborators

> **STATUS: IMPLEMENTATION COMMITTED — INTEGRATOR TODOs REMAIN.** Frame-pass preset policy and pass-descriptor configuration are extracted; orchestration extraction, native tests, and GPU parity remain integrator follow-ups.

**Finding:** L-01  
**Priority:** P1

## Problem and intended change
Extract narrow frame-stage orchestration/services from the oversized renderer without replacing render paths or introducing general GPU memory ownership.

## Implemented in this branch
- [x] Extract `WorldRendererConfig` and its `All`, `Minimal`, and `FilterOnly` policies into `engine/include/Engine/Renderer/WorldRendererConfig.h`.
- [x] Preserve public API and include the new policy header from `WorldRenderer.h`; no Vulkan execution or resource ownership changes.
- [x] Add CPU contract checks in `tests/test_rendergraph_contract.cpp` for all/minimal/filtered presets and independent configurations.
- [x] Inspect the existing implementation, header, and rendergraph contract tests.

- [x] Extract renderer pass-descriptor construction and associated constants (16 descriptor factories) to `engine/src/Renderer/WorldRendererPassDescriptors.inl` via same-translation-unit include, keeping static linkage and exact code bodies.
- [x] Expand CPU pass-preset contract coverage for optional stages and filtered-only gates.

## Integrator TODOs / remaining acceptance gates
- [ ] Trace frame-stage call graph, extract appropriately narrow orchestration collaborators, and preserve order/feature fallback.
- [ ] Test initialization, shutdown, error paths and integrations; the added contract assertions only cover policy.
- [ ] Run CMake/native build and RendergraphContractTests on supported compilers.
- [ ] Run GPU smoke, validation-layer and benchmark before/after tests.
- [ ] Record verification evidence and confirm backwards compatibility and caller-owned GPU lifetime.

## Scope boundary
Do not introduce a GPU memory manager or automatic lifetime ownership; the caller remains responsible for GPU memory. Do not enable experimental rendering paths. No renderer benchmarks or native builds were executed through the connector.

## Handoff
The extraction is deliberately mechanical: descriptor definitions are byte-for-byte carried into a same-translation-unit implementation fragment. This changes source organization, not rendergraph scheduling, shader selection, GPU lifetime or pass activation. Remaining orchestration decomposition, initialization/shutdown integration tests, Windows/Linux CTest, Vulkan smoke/parity and benchmarks are integrator tasks. Native/GPU tests were not run here.
