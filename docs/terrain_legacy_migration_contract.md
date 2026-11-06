# Legacy terrain reference migration contract

Status: **implementation pending**. This document is a migration and regression contract, not a claim that legacy `.tour` references are already fixed.

## Problem

Existing authored scenes may contain `createNode -type terrain` commands and `TerrainPaint -ref` commands. Historical paint references identify a terrain group by **creation order**, through the `kCreateTerrainRefBase` namespace. Replacing repeatable terrain nodes with one world-level terrain setting (see `terrain_world_semantics.md`) without preserving those identities can silently direct paint to the wrong tiles or lose edits.

## Required migration

1. Recognize legacy terrain-create commands on load, in their original command order. Assign each historical terrain group a stable migration identity before any new world-level terrain normalization.
2. Build a **per-load legacy reference map** from old creation-order terrain references to the migrated world terrain dataset plus tile identities. Never reuse the reference counter from an unrelated scene, undo session, or earlier replay.
3. Resolve every historical `TerrainPaint -ref` through that map. Reject unresolved, ambiguous, or cross-world references with a diagnostic that includes the authored command and reference; never silently paint the first terrain.
4. Preserve existing tile geometry, material/height fields, paint operations, ordering, and seam-neighbor effects; keep source-world coordinate transforms unambiguous.
5. Represent new authoring as a world-level terrain enable/configuration record, with tile-addressed paint changes. Disable must not delete data, and tile unloading must not toggle world terrain.
6. Define an explicit policy for old scenes with multiple terrain-create groups. If lossless unification is not possible (conflicting origins, grids, resolutions, or overlapping data), fail with a migration diagnostic rather than overwriting one group.
7. Preserve replay and undo/redo determinism across load/save/load. Do not emit legacy creation-order references from newly saved scenes; retain a compatibility reader for them.
8. Keep the pak-cooking path and the editor replay path consistent. No silent terrain flattening that differs across tools.

## Regression fixtures required before merge

- Legacy one-terrain scene with paint; compare exact affected tile coordinates and channels before and after migration.
- Legacy two-terrain scene where paint refers to each historical create-order ID, including interleaved commands; assert no cross-target painting.
- Missing/stale/invalid `TerrainPaint -ref`: explicit failure, no mutation.
- Multiple legacy groups with incompatible settings: deterministic diagnostic, no data loss.
- Tile-boundary brush stroke: preserve seam neighbors.
- Enable / disable / re-enable: stored tile data and paint unchanged.
- Tile eviction / reload: world terrain enabled state remains unchanged.
- Legacy scene import -> modern save -> reload -> re-export: stable tile data, no legacy refs.
- Copy/merge of scenes containing historical reference numbers: per-scene isolation.
- Undo/redo and crash-recovery replay; verify reference resolution does not depend on unrelated command history.
- Editor and cooker parity on the same fixtures.

## Implementation review gates

- [ ] Identify all parser, serializer, replay, editor history, project build, and cooker consumers of `CreateTerrain`, `TerrainPaint`, and `kCreateTerrainRefBase`.
- [ ] Implement and test the per-load legacy-reference resolver.
- [ ] Migrate data to world-level terrain representation without loss.
- [ ] Implement explicit conflict/error diagnostics.
- [ ] Update modern serializer and compatibility reader.
- [ ] Run targeted tests and a real legacy-scene smoke migration.

**Do not merge as a completed terrain fix until the items above are implemented and verified.**
