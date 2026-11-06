# Cooked world height envelope (first slice)

PakCooker now emits an additional set of 16 RG16-in-RGBA8 height tiles at
`terrain/occlusion/h_XX_ZZ` when `--terrain <dir>` is supplied.

Each tile starts as the authored terrain H-tile, then receives the **highest
world-space Y intersection** of the scene's placed, unskinned static mesh
triangles at each texel center. It uses the already replayed, offline scene
clusters (including placed scene references and their world transforms). The
authored `terrain/h_XX_ZZ` tiles, mask tiles, and terrain/meta remain unchanged.

This is a *2.5D maximum-height envelope*, appropriate as input for distant
heightfield occlusion/horizon computation, **not** a general geometry shadow
replacement. It cannot faithfully represent stacked floors, caves, thin
vertical walls with zero XZ area, alpha-masked geometry, or animated/skinned
meshes. Heights exceeding the source terrain's configured `baseY..baseY+range`
are clamped by the RG16 encoding, so author terrain range accordingly.

The derived tiles are packaged under the existing Terrain pak kind and a
different path, without changing the terrain/meta schema. No renderer
consumer is wired up in this slice: use an explicit lookup of the occlusion
paths, not the regular height tile paths. A follow-up can build a max-height
mipmap/horizon hierarchy and hook up distant sun-shadow sampling.

## Verification checklist

- Cook a terrain-only pak: derived tiles must equal the input H-tiles.
- Cook terrain with a placed raised platform/building: derived tiles must
  rise under the mesh XZ footprint; base terrain heights must not change.
- Verify parent transforms, rotated/nonuniformly scaled instances, multiple
  overlapping roofs, and triangles crossing tile boundaries.
- Verify non-overlapping meshes, meshes below the terrain, degenerate XZ
  triangles and skinned meshes do not modify the derived height.
- Verify `--verify on` and standalone/editor cooking on supported platforms.

Native compile, GPU validation and end-to-end cook verification have not
been performed in this environment; this initial PR is intentionally draft.
