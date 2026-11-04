# Procedural shape source assets

`Engine/Asset/shape_asset.h` defines the versioned `.shape` authoring
format and CPU tessellator. `.shape` assets are first-class package content:
recipe includes recognize them, the catalog emits typed `ShapeKey` ids
(0xBxxx), the cooker validates the text and embeds `PakAssetKind::Shape`
blobs, and `ContentRegistry` exposes the bytes through stable ids or VFS
paths. Editor **New... → New Procedural Shape** creates the source document.
The native `PointList`/`LineList` WorldRenderer pass renders authored
point clouds and splines; triangle primitives use the OBJ/mesh path.

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

**Remaining scope:** viewport point handles/gizmos; automatic authored
triangle-mesh conversion; dedicated point-cloud LOD/splat rendering;
full scene-DAG parenting and rotation for shape placements.

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
from a content-registry/pak read without filesystem access;
`LoadFile` reads at most 8 MiB from the local authoring filesystem and
retains decoded geometry; subsequent file changes do not implicitly reload
the shape. The registry supports 4096 instances, with 65536 points and
65536 line segments per frame through the shared overlay pass. Instances
which cannot fit are skipped whole. GPU vertex data uses the existing per-flight buffer pool; the renderer
caches each flight's last shape revision and skips GPU re-uploads when
retained instances are unchanged. Transient debug-draw sources still
upload every frame. Dedicated per-instance GPU allocations, point-cloud
streaming remain future optimizations.

## Authored placements in `.tour` and packaged scenes

```text
shapeInstance -name "SplineTrail" -source "content/shapes/trail.shape" -t 1 2 3 -color 1 0.8 0.2 -enabled on;
shapeInstance -name "Stars" -source "content/shapes/stars.shape" -t 0 0 0 -color 0.4 0.9 1 -enabled on;
```

The `.tour` writer round-trips `shapeInstance` commands as scene-owned
placements. Names must be unique in the scene; source paths must be
relative `.shape` paths. The cooker writes the placements to an internal
`PakAssetKind::ShapePlacements` blob; the referenced source assets must
also belong to the recipe's owned or dependency pak and be included with
the build.

At runtime, the packaged entry scene loads its placements automatically.
The `ProjectScenes` service binds to `WorldRenderer::Shapes()` through
the application `Context` and instantiates placements when loading an
additive/prefab scene. `RemoveScene` releases the exact handles that
scene owns; prefab-frame updates move its instances. Invalid or missing
shape content fails scene loading rather than silently dropping an instance.

These are **doc-level placement records**, not ordinary `.tour` DAG
nodes or scene-table components yet. The authored placement supports
translation, color and enabled state, not parented rotations/scales.
The per-pak placement sidecar currently represents a single scene's
placements; multi-scene pak-specific sidecars are not implemented.
`ProjectScenes` must be connected to `ShapeScene` before loading
additional scenes (the application `Context` does this automatically).

### Validation

CPU tests: `ShapeAssetTests`, `ShapeSceneTests`, `TourTests`,
`DebugDrawTests`. End-to-end Vulkan capture, pak recipe cook and
multi-scene packaging still need validation.
