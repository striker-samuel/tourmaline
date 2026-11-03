# Immediate-mode debug drawing

Tourmaline's `tour::DebugDrawList` is a CPU-only collection of world-space
wireframe lines. It has no scene-node, collider, Jolt, or GPU handles.

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

`Line`, `Box` (oriented by a glm::mat4), `Aabb`, `Sphere`, `Capsule`,
`Ray`, and `Axes`. All positions use world coordinates; colors use linear
RGB in the range 0–1. Choose `tour::DebugDepthMode::Tested` for normal
occlusion, or `Always` for an X-ray overlay.

The CPU list contains line segments and can be consumed by another renderer,
logger, or test. The WorldRenderer's optional consumer batches equal-color
segments and draws them with its existing debug-line shader in the Main pass,
including HDR exposure compensation and MSAA. Buffers are kept per flight
slot, but their contents are regenerated only while the overlay is enabled.

Each list caps itself at 65,536 segments, with `DroppedLines()` reporting
overflow. `Clear()` resets the list and its overflow count. There are no
persistent shape handles, no scene serialization, and no implicit singleton.

This first implementation intentionally uses flat-color batches; a future
per-vertex-color shader can reduce draw calls for extremely heterogeneous
debug lists without changing the game's debug drawing API.
