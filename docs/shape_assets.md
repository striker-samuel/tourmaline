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

## Persistent renderer instances

`WorldRenderer::Shapes()` exposes a renderer-owned
`tour::renderer::ShapeScene` registry for spline and point-cloud instances.
It holds tessellated local-space geometry, transform, flat color and depth
mode until explicitly removed or cleared. Instance handles contain a slot
index plus generation, so stale handles cannot modify recycled instances.
Instances render in the Main HDR point/line pass even when game debug
drawing is disabled. The native Vulkan `PointList` and `LineList`
pipeline variants are reused.

```cpp
std::string error;
auto cloud = renderer.Shapes().LoadFile(
    "content/shapes/cloud.shape", glm::mat4(1.0f),
    glm::vec3(0.3f, 0.8f, 1.0f), error);
// Or create from already parsed/tessellated source:
auto curve = renderer.Shapes().Add(
    tour::asset::DefaultShapeAsset(tour::asset::ShapeKind::Spline),
    glm::mat4(1.0f), glm::vec3(1.0f, 0.8f, 0.2f), error);
if (curve)
    renderer.Shapes().SetTransform(curve, newTransform);
renderer.Shapes().Remove(cloud);
```

`LoadText(text, transform, color, error)` accepts decoded `.shape` source
from a content-registry/pak read without filesystem access;\n`LoadFile` reads at most 8 MiB from the local authoring filesystem and
retains decoded geometry; subsequent file changes do not implicitly reload
the shape. The registry supports 4096 instances, with 65536 points and
65536 line segments per frame through the shared overlay pass. Instances
which cannot fit are skipped whole. GPU vertex data uses the existing per-flight buffer pool; the renderer
caches each flight's last shape revision and skips GPU re-uploads when
retained instances are unchanged. Transient debug-draw sources still
upload every frame. Dedicated per-instance GPU allocations, point-cloud
streaming and scene-file associations remain future optimizations.

This is the first **persistent scene-renderer instance path**, but NOT
a serialized `.tour` DAG node, cooked `.pak` asset, or a general purpose
scene-component type. Automatic scene-reload / scene-unload ownership and
runtime content-registry integration are still to be added.
