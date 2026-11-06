# Integrator validation matrix — 2026-11-06

**Status: draft / not executed here.** This is a handoff checklist, not a report of passing tests. All relevant PRs must report the exact commit SHA, OS/compiler/SDK, command, result, and links to logs or artifacts.

## P0 — build and compatibility

- [ ] Full Linux GCC and Clang builds including tests, shaders and tools.
- [ ] Full Windows MSVC editor, cook tools, runtime and shaders build.
- [ ] Run all headless CTest targets and attach failures/summary.
- [ ] Confirm all material encoding sites use the global `::KtxEncodeFormat` declaration with the required header visible (previous CI fix commits).
- [ ] Verify no stale generated assets hide an incompatible binary/material serializer layout.

## P0 — terrain migration (separate implementation PR)

- [ ] Legacy `createNode -type terrain` + `TerrainPaint -ref` fixtures replay to the same tiles and channels.
- [ ] Invalid/ambiguous creation-order refs fail explicitly without modifying terrain.
- [ ] Multiple legacy terrain groups and scene merge preserve reference isolation.
- [ ] Save/reload, undo/redo, and editor crash-recovery replay preserve paint.
- [ ] World-enable toggle is independent of per-tile residency.
- [ ] Paint across tile seams; evict/reload; verify editor/cooker parity.
- [ ] Do not mark complete until corresponding implementation PR adds code and fixtures.

## P0 — bake input fingerprints and light direction

- [ ] Cook, GiBaker, and pak freshness checks yield identical environment/light/grid hashes for identical authored inputs.
- [ ] Compare scenes with no lights, only synthesized `sun/world`, runtime-only lights, and authored static directional lights.
- [ ] Change authored sun azimuth/elevation, irradiance and light color independently; expected hashes change.
- [ ] Confirm synthesized sun is not double-counted and inverse-trig reconstruction cannot perturb authored environment hashes.
- [ ] Create GI and cubemap captures, rebuild unchanged content and verify no false rejection.
- [ ] Confirm generated sun direction convention (toward sun vs incoming ray), coordinate handedness and shadow/GI agreement.

## P1 — materials / cook

- [ ] Run `ProjectPipelineTests` and related material packing tests.
- [ ] Encode/decode BC7 scalar, BC5 normal, BC1 emissive, and RGBA8/BC3 cases in a full project cook.
- [ ] Verify linear scalar/normal sampling and appropriate sRGB color/emissive sampling on GPU.
- [ ] Validate recipe-change invalidation, generated file cleanup, missing textures, incompatible sizes, duplicate directives and older unmodified materials.
- [ ] Compare source and cooked channel selectors; verify legacy material blobs continue to load.

## P1 — precision / scene memory

- [ ] Run scene-table, double-float, scene-memory-binding and relevant serialization tests.
- [ ] Validate +/− large absolute coordinates, fractional positions, deep parent/child transforms and scene merges.
- [ ] Confirm v1 double scene format round-trips and legacy unversioned float scene files remain readable.
- [ ] Verify unsupported/truncated versioned inputs reject cleanly; older executables are not advertised as forward-compatible.
- [ ] Exercise bind/detach and external capacity limits, pointer lifetime and mutation journaling.
- [ ] Confirm float semantic views cannot overwrite authoritative doubles inadvertently.
- [ ] Run GPU camera-relative publication parity on real Vulkan hardware, including multi-view/frozen culling.

## P1 — editor / runtime

- [ ] Live sphere/cube material preview: resize, material switches, docking, Vulkan layout synchronization, cleanup.
- [ ] Component remove/undo/redo, authored-name breadcrumbs and optional material component behavior.
- [ ] Missing materials consistently show the pink error material.
- [ ] Verify `--bench_turn` with positive/negative rates, `--bench_move`, warm-up and frame budgets.

## P2 — performance

- [ ] Run representative 4K, 32K and 105K scene-content workloads with identical settings.
- [ ] Capture CPU stage Optick aggregates and GPU times; record median, p95 and warm-up.
- [ ] Distinguish synthetic CPU-only harness measurements from real cooked-scene GPU runtime measurements.
- [ ] Compare before/after scene storage and transform changes for memory, frame-time and streaming regressions.

## Signoff template

| Gate | Commit SHA | Platform | Command / capture | Result | Artifact / issue |
| --- | --- | --- | --- | --- | --- |
| Build | TBD | TBD | TBD | NOT RUN | TBD |
| Terrain migration | TBD | TBD | TBD | NOT RUN | TBD |
| Bake consistency | TBD | TBD | TBD | NOT RUN | TBD |
| Material smoke cook | TBD | TBD | TBD | NOT RUN | TBD |
| Precision / memory | TBD | TBD | TBD | NOT RUN | TBD |
| GPU/editor parity | TBD | TBD | TBD | NOT RUN | TBD |

Do not turn unchecked gates into PASS merely because their implementation commits were merged.
