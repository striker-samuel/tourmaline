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
different path, without changing the terrain/meta schema.

## Hierarchy and directional sun traversal

PakCooker additionally emits `terrain/occlusion/max_pyramid`: a
2048×1024 R16_UNORM raw little-endian atlas containing **maximum**
height reductions of the derived tiles, not filtered/averaged heights.
The first level is 1024×1024 (each texel covers 4×4 source height
texels), and levels 3..12 are packed horizontally to its right.
See `terrain_occlusion.h` for the exact layout. Hierarchy maxima include
object geometry and respect source H-tile boundaries.

`World::LoadTerrainFromPak` validates the extra tiles and pyramid together.
Old paks with the original 273 terrain blobs continue to load; new ones
have 290. `TerrainField::BuildFromSource` uploads the pyramid as a
second bindless R16_UNORM image. The terrain fragment shader's directional
sun leg uses a coarse-to-fine **64m → 16m → 4m** traversal, skipping a
cell only when its maximum surface height is lower than the minimum
rising sun-ray Y over that cell. The minimum of the result and the
existing cascaded PCF/EVSM shadow receiver preserves short-range details.
Without the cooked pyramid, the shader falls back to the original
fixed-step terrain-only shadow raymarch.

The 4m leaf cell is a *conservative height column*, not an exact triangle
intersection: false-positive shadowing along silhouettes is possible.
The initial rasterizer samples mesh triangles at terrain-texel centers:
subtexel-width geometry may be missed entirely. Dynamic geometry,
vegetation cutouts, interior/overhang topology, sun penumbrae and non-terrain
mesh receivers are not supported by this path. The current ray extent
still follows `kTerrainMarchDist = 1500m` and starts at 30m.

A future improvement is a 1m leaf verification against the complete
occlusion H-tile, or coverage-conservative mesh voxelization, plus
horizon filtering to soften silhouettes. This PR does not claim
physically exact distant geometry shadows.

## Verification checklist

- Cook a terrain-only pak: derived tiles must equal the input H-tiles.
- Cook terrain with a placed raised platform/building: derived tiles must
  rise under the mesh XZ footprint; base terrain heights must not change.
- Verify parent transforms, rotated/nonuniformly scaled instances, multiple
  overlapping roofs, and triangles crossing tile boundaries.
- Verify non-overlapping meshes, meshes below the terrain, degenerate XZ
  triangles and skinned meshes do not modify the derived height.
- Verify `--verify on` and standalone/editor cooking on supported platforms.
- Run `TerrainOcclusionTests` to check parent maximum invariants and seams.
- Compare the fixed-step fallback against the new hierarchy in GPU captures;
  especially grazing sun angles, near terrain boundaries and buildings.

Native compile, GPU validation and end-to-end cook verification have not
been performed in this environment; this PR remains draft pending checks.
