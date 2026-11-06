# World-owned terrain: replace legacy create-order references

**Status: implementation pending.** Project terrain content is currently test scenes that can be reauthored. **No backwards-compatibility or migration support is required.** This specification does not implement the parser/editor/runtime changes.

## Goal

Implement the world-level terrain architecture described in `docs/terrain_world_semantics.md`. Terrain is a **single logical world feature**, not a set of independently created terrain scene nodes. Tiles remain the unit of persisted height/material/paint data and streaming residency.

## Required implementation

1. Replace repeatable `EditVerb::CreateTerrain` / `createNode -type terrain` authoring with explicit world terrain enable/configuration operations. One terrain definition per world.
2. Remove the creation-order `kCreateTerrainRefBase` reference dependency from new `TerrainPaint` records. Paint must address the world terrain and affected tiles deterministically; do not bind to a historical terrain-group insertion ordinal.
3. Update the `.tour` parser, exporter, command replay, editor undo/redo and cook pipeline **together**, with one documented representation. Old terrain syntax/refs can be rejected with clear errors; do not add a compatibility reader or data migration machinery.
4. World-level enable/disable must preserve tile storage. Missing or unloaded tiles are separate from world terrain enablement.
5. World-space painting must apply to the correct tiles, including seam neighbors, with stable paint results across save/load and undo/redo.
6. Delete obsolete terrain-group UI and code paths and update example/test scenes to the new representation.

## Acceptance tests

- [ ] Enable terrain, configure world parameters, save, reload, and compare exact values.
- [ ] Disable and re-enable without losing tile heights/material/paint.
- [ ] Paint across a tile seam; test all affected tile data.
- [ ] Evict/reload tiles with terrain still enabled.
- [ ] Reorder unrelated scene commands: terrain paint targets remain unchanged.
- [ ] Repeat replay, undo/redo and save/reload; verify deterministic results.
- [ ] Cook and editor use the same world-terrain representation.
- [ ] Legacy terrain syntax produces a clear actionable diagnostic, unless intentionally removed entirely.
- [ ] Reauthor/update any repository test scenes still using historical `createNode -type terrain` and `TerrainPaint -ref`.

## Explicit non-goals

- Supporting old terrain files or historical creation-order references.
- Lossless migration of old terrain groups.
- Keeping the old terrain-node authoring workflow.

Do not mark this draft complete until the actual implementation, fixture updates, and tests have landed and passed.
