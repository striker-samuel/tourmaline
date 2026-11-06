# Integrator validation matrix — 2026-11-06

**Draft; no tests executed by this PR.** Record exact SHA, toolchain, command, result and logs.

## P0 — builds

- [ ] Linux GCC and Clang builds (engine, editor, tools, tests, shaders).
- [ ] Windows MSVC builds and smoke runs.
- [ ] Headless CTest suite, including parser/project pipeline tests.

## P0 — new world-level terrain model (PR #225)

- [ ] One world-level terrain enable/configuration record, no repeatable terrain group authoring.
- [ ] New paint commands are independent of historical creation-order refs (`kCreateTerrainRefBase`).
- [ ] Parser, exporter, replay, undo/redo and cooker agree about the new format.
- [ ] Tests validate save/load, disabled/enabled with tile data preserved, seam painting and tile streaming eviction/reload.
- [ ] Scene-command reordering does not retarget paint.
- [ ] Reauthor/update repository test scenes and fixtures for the new terrain format.
- [ ] Unsupported historical terrain syntax fails clearly; **legacy migration is not required**.

## P0 — sun / bake fingerprints

- [ ] Cook, GiBaker and Pak agree on environment/light/grid fingerprints.
- [ ] Check zero-light, generated `sun/world`, and authored static directional cases.
- [ ] Verify authored sun direction/intensity edits change the relevant fingerprints.
- [ ] Produce and reload GI/cubemap bakes; verify no false freshness rejection.
- [ ] Confirm sun direction conventions between editor, shadowing and GI.

## P1 — material packing and cooking

- [ ] Run `ProjectPipelineTests` and full project cooking.
- [ ] Exercise BC7 scalar, BC5 normal, BC1 emissive and RGBA8/BC3 alternatives.
- [ ] Decode GPU assets and check channel selectors and linear/sRGB usage.
- [ ] Validate recipe invalidation and malformed recipe diagnostics.

## P1 — scene precision / semantic memory

- [ ] Scene-table, double-float and semantic binding tests.
- [ ] Large signed fractional translations, nested hierarchies, merge and serialization.
- [ ] GPU publication parity under camera-relative/multiview rendering.
- [ ] External bind/detach lifetime/capacity and dirty-state journal coverage.
- [ ] Check float semantic views do not silently overwrite authoritative double transforms.

## P1 — editor/runtime

- [ ] Live sphere/cube material preview Vulkan resource transitions and resizing.
- [ ] Undoable Material component removal, breadcumbs and optional binding.
- [ ] Pink error-material behavior for missing assets.
- [ ] `--bench_turn` positive/negative and combined `--bench_move`.

## P2 — profiling

- [ ] Repeat representative CPU/GPU benchmarks with fixed scenarios and environments.
- [ ] Capture CPU stage timings and GPU execution; compare medians/p95.
- [ ] Distinguish synthetic CPU harness results from real cooked-scene measurements.

## Signoff

| Gate | Commit SHA | Platform / command | Result | Logs |
| --- | --- | --- | --- | --- |
| Builds | TBD | TBD | NOT RUN | TBD |
| World terrain | TBD | TBD | NOT RUN | TBD |
| Sun / baking | TBD | TBD | NOT RUN | TBD |
| Material cook | TBD | TBD | NOT RUN | TBD |
| Scene precision | TBD | TBD | NOT RUN | TBD |
| GPU/editor | TBD | TBD | NOT RUN | TBD |

Do not treat implementation commits or unchecked gates as passing tests.
