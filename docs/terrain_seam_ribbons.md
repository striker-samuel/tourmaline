# Low-level tiled-terrain seam bake primitive

`Engine/Rendergraph/terrain_seam_ribbon.h` remains an isolated, GPU-less
helper for the world-tiled `TerrainHField` asset representation.
It accepts world-space triangle soup, locates heightfield crossings, and
emits terrain-conforming strips with per-vertex fade and terrain normals.

This primitive is useful to standalone cookers that already hold the tiled
RG16 heightfield. It does not register its own renderer path or manage assets.

**For the editor-integrated geometry workflow, see
[Intersection geometry tools](intersection_geometry_tools.md).**
That editor path operates on `placement::HeightfieldData` from the *live
painted terrain group*, stitches segments into polylines, supports bounded
spline smoothing and skirts, and exports glTF assets with alpha coverage.
The standalone tiled-terrain helper is intentionally not used by the
painted-terrain editor route, because these are different heightfield
representations in Tourmaline.

The helper's v1 topology policy skips coplanar, tangent or >2-contact
triangles, does not stitch neighboring segments, and conforms only to
terrain. Its regression target is `TerrainSeamRibbonTests`.
