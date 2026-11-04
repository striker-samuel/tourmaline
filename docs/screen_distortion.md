# Screen-space distortion (HDR)

`smvk::Distortion` is a reusable pre-tonemap render-graph effect. It samples
an HDR scene into a separate color target, displacing screen UVs by a vector
field. The caller chooses between an authored flow map and procedural heat
shimmer; the renderer remains independent of the effect's use case.

## Integration

```cpp
#include <Engine/Rendergraph/distortion.h>

smvk::Distortion distortion;
smvk::DistortionDesc settings{};
settings.mode = smvk::DistortionMode::Heat;
settings.strengthPx = 5.0f;
settings.speed = 2.0f;
settings.frequency = 0.035f;
settings.centerX = 0.5f;
settings.centerY = 0.65f;
settings.radiusX = 0.35f;
settings.radiusY = 0.3f;

// Recreate/ensure after resize or HDR format changes, as with MotionBlur.
if (distortion.Ensure(ctx, rg, hdrExtent, settings)) {
    distortion.AddDistortionPass(rg, hdrView, hdrBindless,
                                 linearSamplerIndex, hdrExtent,
                                 secondsSinceStart);
    // Feed distortion.OutputView() and OutputBindless() into later passes
    // (bloom, motion blur, tone mapping) instead of the original HDR scene.
}
// Shutdown: distortion.Destroy(rg), before destroying ResourceManager.
```

For an authored distortion map, use `DistortionMode::Field`, set
`fieldView` / `fieldBindlessIndex` on `AddDistortionPass`, and provide an
RGB texture where R/G encode signed displacement with 0.5 as neutral and B
masks the displacement (1 = active, 0 = none). Adjust `fieldScale`.
If the field is unavailable, the pass copies the original scene.

The ellipse `centerX/Y`, `radiusX/Y`, and `edgeSoftness` determine the
affected screen region. `strengthPx` is pixel displacement at the source
resolution; setting it to zero is a clean identity operation. Time is
explicitly supplied in seconds; the effect does not own a clock.

## Render-graph contract

* Insert **after scene HDR rendering and before bloom/tonemap**, avoiding
  feedback loops: input and output must be different images.
* The effect owns a matching-format, matching-extent HDR output and a bindless
  sampled view. `Ensure` recreates these when the dimensions or format change.
* `AddDistortionPass` declares scene and optional flow-map input barriers and
  its output color attachment. The caller is responsible for registering the
  flow map as a logical external view and binding it as a sampled image.
* Shader compilation follows the existing `engine/shaders/*.frag.hlsl` glob;
  fullscreen VS is shared with tonemap.
* This is a **full-frame** pass. Localized heat distortion is expressed by the
  ellipse, not by subrect render-graph/scissor settings. Depth-based occlusion,
  object-driven distortion emitters, and UI/editor controls are future work.

## Verification checklist

* With strength zero, the output matches the input (within sampling precision).
* Test both modes at 1280x720 and 1920x1080, including resizing between them.
* Test no flow map, neutral flow map, B=0, and edge-of-screen field vectors.
* Check the output in the HDR chain before tonemapping and verify no read/write
  image aliasing or validation errors.
* Compare frame times with distortion disabled and enabled.

GPU compilation and visual validation require a Vulkan/DXC-capable machine.
