# Procedural shape source assets

`Engine/Asset/shape_asset.h` provides a **CPU-only** source format and
tessellator. This first increment does not yet register `.shape` in the
project asset database, pak/catalog cooker, or Vulkan
point/line rendering. The editor asset browser **New... → New Procedural Shape**
menu creates editable source documents, but they do not render yet.

The supported kinds are `spline` (open/closed Catmull–Rom, indexed lines),
`point_cloud` (authored XYZ positions, point topology), and `plane`,
`box`, `sphere`, `cylinder`, `cone` (indexed triangles).
Coordinates are local Y-up. Plane/box span 2 units; round primitives
use positive `radius` and `height`; `segments` controls samples.

Example `.shape` document:

```text
shape 1
kind spline
closed 0
segments 16
radius 1
height 2
point -2 0 0
point -1 1 0
point 1 1 0
point 2 0 0
```

Point cloud example:

```text
shape 1
kind point_cloud
point 0 0 0
point 0 1 0
point 1 0 0
```

API: `ParseShapeAsset(text, shape, error)`,
`SerializeShapeAsset(shape)`, `DefaultShapeAsset(kind)`,
`ValidShape(shape, error)`, `TessellateShape(shape, geometry, error)`.
The generic asset inspector shows an oblique CPU wireframe/point preview and edits spline/point-cloud XYZ coordinates, spline closure, and primitive dimensions, with Save/Revert. Tessellation rejects assets generating more than 250,000 vertices to cap memory use. Triangle primitives can also be exported as Wavefront `.obj` source meshes through **Export OBJ...** (save the `.shape` first); the existing OBJ importer can then process the exported file. Spline and point-cloud types deliberately do not use the triangle OBJ conversion path.

The parser rejects unknown versions/fields, invalid dimensions and
nonfinite point coordinates, preserving the previous output on failure.
Tessellation likewise preserves the output on invalid input.

**Next integration steps:** viewport point-handle/gizmo editing;
per-project identity/meta registration; triangle mesh conversion into the
existing geometry importer; separate Vulkan line/point draw pipelines;
cooked persistence and runtime loading. This PR establishes the source
data and CPU geometry only.
