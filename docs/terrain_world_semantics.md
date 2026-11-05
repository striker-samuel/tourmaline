# World-level terrain semantics (proposal)

## Intent

Terrain is **enabled or disabled for a world**, not created as a collection
of independent scene nodes. Tiles are the *storage and streaming units* of
one world terrain dataset; they do not own separate terrain enablement
states.

The distinction is between **logical ownership** and **physical residency**:

| Concern | Scope | Meaning |
|---|---|---|
| Terrain enabled | World | One logical on/off setting for the entire world |
| Height, material and paint data | Tile | Independently persisted and loaded data |
| Residency | Tile | Tiles may be unloaded even when world terrain is enabled |
| Terrain editing | World coordinates | Edits identify affected tiles, including seam neighbors |

An unloaded or missing tile does **not** mean terrain is disabled for
that part of the world. A world with terrain disabled does not schedule
terrain streaming or render terrain, regardless of stored tile data.
Switching terrain back on must not discard existing authored tiles.

## Desired authoring behavior

- Expose **Enable terrain** / **Disable terrain** as world-level
  operations or properties, rather than **Create Terrain** as an
  arbitrarily repeatable scene-node command.
- Author one terrain configuration per world (extent, origin, tile grid,
  resolution and any other world-wide parameters). Editing this
  configuration is distinct from streaming tiles.
- Store terrain heights, materials and paint in separately addressable
  tiles. Streaming and cache eviction never change the world setting.
- An absent tile is a data/residency case, not a second kind of
  world-level toggle. Define explicit fallback/error behavior for
  missing tile data.
- A tile selected for editing is a *view into the world terrain*, not an
  independently creatable/deletable terrain entity. Destructive tile
  clearing, where supported, should modify tile data without disabling
  terrain.

## Existing implementation and migration considerations

Today the editor's `EditVerb::CreateTerrain` produces a terrain group
with child tiles and exports a `createNode -type terrain` command in
the `.tour` layer. `TerrainPaint` references the created group through
the creation-order `kCreateTerrainRefBase` namespace. This is the
node-creation semantic this proposal intends to retire.

A follow-up implementation should:

1. Introduce an explicit world terrain enable/configuration record in
   the authored scene and cooked runtime representation.
2. Keep per-tile data identifiers stable and independently streamable.
3. Change the editor command/undo UI to operate on world terrain
   state; keep paint operations tile-addressed.
4. Define compatibility when loading older `.tour` files containing
   `createNode -type terrain` and `TerrainPaint -ref` commands,
   preserving tile content and paint before removing the old syntax.
5. Test enable/disable round-trips, multi-tile painting across seams,
   tile eviction/reload, and migration of existing authored terrain.

**Status:** Design clarification only. This document does not change
the current editor, `.tour` parser or runtime behavior.
