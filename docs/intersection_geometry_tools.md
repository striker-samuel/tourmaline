# Intersection curves, terrain seam ribbons and skirts

This feature is an **offline editor baker**. It is deliberately independent of
deferred lighting: a baker extracts contact curves on the CPU, creates new
triangle meshes, and exports glTF source that flows through Tourmaline's
existing importer/cooker and forward-lit renderer.

## Editor workflow

1. Load a scene with a painted terrain group (or select its terrain tile).
2. Open **Geometry > Intersection curves / seam bake...**.
3. Set **Source mesh A (.obj)** to a world-space, Y-up mesh file.
4. Choose **Mesh / painted terrain**. The selected terrain group (or the
   active World Map group) is the receiver; the extraction uses the **current
   painted heightfield**, not an old source heightmap.
5. Set sample spacing, endpoint weld tolerance, ribbon width and optional
   spline smoothing. Click **Extract intersection curves**. Inspect the XZ
   polyline preview; ambiguous faces are reported.
6. Choose **Terrain-conforming blend ribbon** or **Vertical skirt**.
   Click **Build bake geometry**.
7. Choose an output location under project content (e.g.
   `content/geometry/rock_terrain_seam.gltf`) and click **Export for
   Tourmaline (.gltf + .bin)**.
8. Import the generated glTF as normal project source geometry and attach
   your seam material. The generated geometry needs no new runtime pipeline.

The tool also exports OBJ line primitives for diagnostic curves and OBJ
triangles for DCC work. The alpha weights are **portable in glTF**, through
`COLOR_0.a`, but **not guaranteed through OBJ** (OBJ vertex color is a
nonstandard extension). Prefer glTF for production use.

The exported glTF contains POSITION, NORMAL, TEXCOORD_0 and COLOR_0
plus uint32 indices; its source material is double-sided AlphaMode BLEND.
Its vertex color RGB is white and alpha is the baked fade weight. The
forward-lit material path has been updated to multiply base alpha by
vertex-color alpha on nonlayered materials. This is also correct glTF
semantics for other imported geometry.

## Mesh/mesh intersection curves with CGAL

Configure an editor build with CGAL 6.2 or newer:

```sh
cmake -S . -B out/build/geometry -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTOURMALINE_EDITOR_CGAL_INTERSECTIONS=ON \
  -DCGAL_DIR=/path/to/cgal/lib/cmake/CGAL
cmake --build out/build/geometry --target Editor
```

Your platform's ordinary Vulkan, KTX, and other Tourmaline dependencies
are still required (see README). Without that explicit flag, regular
engine and editor builds do not include CGAL.

In the panel choose **Mesh / mesh (CGAL)** and provide two OBJ files.
Both must be triangulatable and in the same world-space coordinate
system. The backend builds indexed CGAL Surface_mesh instances,
rejects malformed/nonmanifold face insertion and self-intersecting input,
calls `CGAL::Polygon_mesh_processing::intersection_polylines()`, and
returns the same engine-level `CurveSet` type used by the terrain baker.
The curves can be exported as OBJ lines or turned into **vertical skirts**
with optional corner smoothing. Mesh/mesh surface-conforming ribbon
offsets are intentionally not inferred: they require a receiver-surface
projection operation, unlike a simple heightfield.

## General geometry API

`Engine/Geometry/intersection_curves.h` owns the non-CGAL API:

- `LoadObjTriangles` — a simple world-space OBJ triangle reader
- `StitchSegments` — weld/sort edge contacts into open/closed polylines
- `ExtractTerrainIntersection` — sample mesh triangle edges against the
  selected heightfield, bisect crossings, stitch the contact graph
- `SmoothPolyline` and `Resample` — optional derivative curve processing
- `BakeTerrainRibbon` — a three-row edge-fading conforming strip
- `BakeVerticalSkirt` — a normal-bearing vertical skirt
- `SaveCurvesObj` and `SaveRibbonObj` — interchange exports
- `SaveRibbonGltf` (in `seam_gltf.h`) — cooked-asset input with alpha

`Engine/Geometry/cgal_intersection.h` provides the optional CGAL backend
when compiled with `TOURMALINE_EDITOR_WITH_CGAL` (set only by editor
CMake). The core types do not depend on CGAL or Vulkan.

## Geometry and shading behavior

- Every ribbon centerline follows the extracted contact curve. Ribbon
  vertices are snapped to the sampled terrain surface and lifted slightly
  to avoid z fighting.
- Ribbon width controls the overall cross section. Edge weights fade
  0 -> 1 -> 0, leaving the underlying terrain visible outside the seam.
- Smoothing is optional and applies only to **derived baked geometry**,
  not to the exact diagnostic intersection curve. Smoothing can move the
  centerline away from the true contact; use zero smoothing when precision
  matters.
- Long segments are resampled before ribbon/skirt tessellation.
- Mesh/mesh skirts follow CGAL curves. Skirts add vertical geometry but
  do not change either source mesh's topology.
- The seam overlay's appearance comes from its material: transparent
  brown dirt, moss, rubble or road-edge material. This is a **shading
  transition** and hides surface discontinuities; it does not calculate
  a boolean union or remove an overhanging silhouette.
- Geometry is exported in **world space**. Do not reapply the source
  object's transform when placing the imported seam.

## Limitations and diagnostics

- Terrain extraction accepts ordinary crossings with exactly two
  distinct contacts per mesh triangle. Coplanar contact regions,
  multi-crossings and tangent-only cases are flagged/skipped; an
  ambiguous count is shown in the UI.
- Root finding is bounded by `maxEdgeSteps` and `step`; large triangles
  or high-frequency heightfields may need smaller steps. Extremely
  discontinuous terrain isn't represented by a heightfield.
- Endpoint welding uses a deterministic quantized spatial key. At cell
  boundaries, very close contacts can still end up in separate paths.
- Nonmanifold junctions terminate paths rather than picking an arbitrary
  continuation. This makes T-junction geometry explicit in the preview.
- The optional CGAL backend assumes valid, non-self-intersecting triangle
  meshes. Invalid topology is reported instead of silently repairing.
- A generated scene mesh must be imported and assigned a material.
  The baker does not silently modify .tour files or overwrite source
  geometry. Exported assets are normal content that can be versioned.
- Curve preview is XZ only. It is not a live on-mesh gizmo.
- No regression execution has been performed as part of this change;
  validation is deferred to the integrator as requested.

## Licensing

CGAL's **Polygon Mesh Processing** implementation is licensed
`GPL-3.0-or-later OR LicenseRef-Commercial`. The source is not vendored,
and the dependency is OFF by default. Building/distributing a CGAL-enabled
editor must be evaluated under CGAL's applicable license or covered by
a separate commercial license. Separating it from a normal runtime build
is not, by itself, an exemption from GPL obligations.

Relevant documentation:
- https://doc.cgal.org/latest/PMP_Boolean_operations/group__PMP__corefinement__grp.html
- https://github.com/CGAL/cgal/blob/main/Polygon_mesh_processing/include/CGAL/Polygon_mesh_processing/intersection.h

## Further extensions

Polyline output is deliberately reusable for procedural caps, foundation
skirts, wall/floor transitions, crack masks, stitching, road shoulders and
future geometry-guided decal placement. A future editor enhancement can
attach persistent source GUIDs to baked curves for automatic regeneration.
