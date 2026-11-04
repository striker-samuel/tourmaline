# Immediate-mode debug drawing

Tourmaline's `tour::DebugDrawList` is a CPU-only collection of world-space
wireframe lines and points. It has no scene-node, collider, Jolt, or GPU handles.

Use it to inspect physics, navigation, triggers, AI paths, or custom runtime
data. A producer can fill a list without constructing a renderer.

```cpp
#include <Engine/Renderer/DebugDraw.h>

// In the game setup, after acquiring the WorldRenderer:
renderer.SetDebugDrawSource([&](tour::DebugDrawList& draw) {
    // This callback is invoked only while debug drawing is enabled.
    for (const auto& collider : myPhysics.Colliders())
        draw.Box(collider.worldTransform, collider.halfExtents,
                 glm::vec3(0.3f, 1.0f, 0.3f));
});
renderer.SetDebugDrawEnabled(true);
```

In the viewer, press **Tab** and open **Debug > Overlays > Game debug lines**
to toggle the overlay. The checkbox defaults to **off**. A custom host can
use `SetDebugDrawEnabled` instead. The callback observes the game's current
CPU state at render time and needs no GPU representation or synchronization.

## Shapes

`Point`, `Line`, `Box` (oriented by a glm::mat4), `Aabb`, `Sphere`, `Capsule`,
`Ray`, and `Axes`. All positions use world coordinates; colors use linear
RGB in the range 0–1. Choose `tour::DebugDepthMode::Tested` for normal
occlusion, or `Always` for an X-ray overlay.

The CPU list contains line segments and point vertices and can be consumed by another renderer,
logger, or test. The WorldRenderer's optional consumer batches equal-color
segments and points and draws them with the existing position-only shader and\n**native Vulkan LineList / PointList pipelines** in the Main pass,
including HDR exposure compensation and MSAA. Buffers are kept per flight
slot, but their contents are regenerated only while the overlay is enabled.

Each list caps itself at 65,536 segments and 65,536 points, with\n`DroppedLines()` and `DroppedPoints()` reporting overflow. `Clear()` resets the list and its overflow count. There are no
persistent shape handles, no scene serialization, and no implicit singleton.

This first implementation intentionally uses flat-color batches; a future
per-vertex-color shader can reduce draw calls for extremely heterogeneous
debug lists without changing the game's debug drawing API.

## Shape assets into the RHI draw stream

The RHI already maps `smvk::PrimitiveTopology::LineList` and `PointList`
to Vulkan primitive topologies. `WorldRenderer` now submits both as grouped
position-only vertex draws; no triangle expansion or meshlet cooking occurs.
The point path uses Vulkan's rasterized point size (typically one pixel).

`Engine/Renderer/ShapeDebugDraw.h` provides `AppendShapeDebug` for
`.shape` splines and point clouds. The caller supplies a world transform
and color. For example:

```cpp
renderer.SetDebugDrawSource([&](tour::DebugDrawList& draw) {
    std::string error;
    tour::renderer::AppendShapeDebug(myShape, myWorldTransform,
        glm::vec3(0.2f, 0.9f, 1.0f), draw, error);
});
renderer.SetDebugDrawEnabled(true);
```

This is an **opt-in debug/drawing overlay**, not automatically a cooked
scene asset. Triangle shapes are rejected by this adapter (use mesh/OBJ
instead). The adapter refuses shapes that would overfill the debug list,
without partially emitting them. Renderer GPU validation and screenshots
are still required on a Vulkan-capable machine.
