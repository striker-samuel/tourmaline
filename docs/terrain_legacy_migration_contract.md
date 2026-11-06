# Preserve existing terrain paint references (no format migration)

**Status: implementation and validation pending.** This PR is a backward-compatibility contract; it does **not** change the `.tour` format or implement the fix yet.

## Non-negotiable compatibility rule

Existing repository scenes containing `createNode -type terrain` and `TerrainPaint -ref` **must continue to load, replay, save, and cook with identical terrain and paint data**. Historical references are creation-order based (the `kCreateTerrainRefBase` namespace). Do not rewrite their meaning, renumber them, or silently merge multiple terrain groups.

**Out of scope:** converting legacy files to a new world-level terrain format; removing `CreateTerrain` / `TerrainPaint` syntax; converting historical group references into world-global identifiers; normalizing multiple old terrain groups; changing serialized paint records. Those changes require a separate, explicitly opted-in versioned migration with a proved lossless round-trip.

## Required compatibility fix

1. Preserve the current on-disk commands and reference numbering, including command order, when loading and re-saving legacy scenes. Keep the existing legacy parser/serializer path.
2. If internal code changes require reference resolution, resolve legacy paint refs against the terrain group created by the corresponding historical create command **within the same scene/replay session**. Do not reuse mapping state across unrelated scene loads. Do not replace all old references with the first or sole world terrain.
3. Maintain ordering and exact terrain height/material/paint fields, target group, tile coordinates and brushes. No silent fallback or skipped command when a reference cannot be resolved; report the offending command and value without writing partial paint.
4. Keep undo/redo, recovery replay, scene copy/merge, and cooker loading behavior compatible. Protect existing scenes as authored; do not force a new world-level representation when saving legacy content.
5. The world-level terrain semantics proposal is **independent**. It must not alter existing files or their meaning until a distinct migration has been designed, implemented, and validated.

## Required regression fixtures

- [ ] Load and replay a real historical repository `.tour` scene; capture baseline terrain heights/materials/paint before changing code.
- [ ] Save/reload it using the existing format; verify byte-level command/reference stability where feasible and semantic parity of all tile data.
- [ ] Two terrain create commands with interleaved paints: each historical `-ref` must address its original group.
- [ ] Legacy refs across separate world/scene loads: mapping state must not leak.
- [ ] Unknown/stale refs: diagnostic and no accidental target mutation.
- [ ] Tile seams, brush order, undo/redo, autosave/recovery, scene copying/merging.
- [ ] Editor and cooker replay produce the same results and report invalid references consistently.
- [ ] Existing fixture outputs remain unchanged unless a separately approved compatibility fix explicitly corrects a previously demonstrated bug.

## Integrator gate

- [ ] Trace parser, serializer, replay, editor and cooker references to `kCreateTerrainRefBase`, `CreateTerrain`, and `TerrainPaint`.
- [ ] Implement the narrow backward-compatible fix and tests, **without editing format definitions**.
- [ ] Execute legacy-scene tests and compare baseline outputs before marking complete.

Do not merge this draft as an implementation: it currently contains only this compatibility specification.
