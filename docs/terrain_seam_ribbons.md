# Terrain-only seam ribbon bake (initial primitive)

This is the first, intentionally narrow engine-level step toward automatic
mesh/terrain discontinuity blending. The CPU-only baker lives in
`engine/include/Engine/Rendergraph/terrain_seam_ribbon.h` and consumes the
existing `TerrainHField` plus world-space triangle vertices.

## Output

`BakeTerrainSeamRibbons(terrain, triangles, options)` emits a mesh made of
terrain-conforming six-vertex, four-triangle ribbons. Each detected mesh/
heightfield intersection segment becomes a strip with zero coverage at both
outer edges and full coverage at its centerline. The returned vertex normals,
UVs, and weights are suitable inputs to a baked overlay-material workflow.

The baker does not require a G-buffer, deferred shading, CGAL or Vulkan.
It does **not** yet insert the output into scene cooking, editor UI, or render
passes. Consumers must provide a material, vertex-format conversion, and
normal blending/alpha handling. It is an engine geometry primitive, not a
finished interactive decal feature.

## Limitations

* Mesh triangle vertices must be in **world space**, with three per face.
* The terrain must be valid and use the existing sampled heightfield.
* Edge sampling finds multiple crossings but v1 only accepts triangles
  with exactly two distinct contacts; ambiguous/coplanar cases are skipped.
* Contact segments are not yet welded into long splines; independent strips
  can leave corner artifacts. Welding, curvature-aware conforming and
  editor preview are follow-up work.
* A heightfield cannot model undercuts/overhangs. The ribbon only conforms
  to terrain, not to the intersecting mesh.
* This is **material seam masking**, not a topological mesh union or a fix
  for silhouette intersections.
* Very large triangles require a small `sampleStep` and sufficiently large
  `maxSamplesPerEdge` to avoid missed terrain crossings.
* `lift` prevents depth fighting only approximately; integration must
  handle alpha and depth appropriately.

## Next stages

1. Weld adjacent line segments and build smooth, adaptive polylines.
2. Add a non-destructive editor bake command and preview with export to the
   existing cooked mesh pipeline.
3. Blend receiver and source materials within the existing Forward+ lit hook.
4. Add general mesh/mesh intersection curves behind an optional, license-
   compatible backend; expose curves to skirting and seam generation.
