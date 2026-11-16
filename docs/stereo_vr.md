# Stereo VR view foundation (double-precision worlds)

This first increment provides `Engine/Camera/stereo_view.h`, a CPU-only pair of eye cameras. It does **not** claim headset/OpenXR support yet.

## Contract

- Start with a double-precision world-space head pose, head-local per-eye offset and optional orientation, and each eye's runtime-supplied projection. Do not synthesize a shared symmetric VR projection.
- Transform head-local eye offsets in double precision; **never** derive the two eyes by offsetting an already-rounded float32 world camera.
- Snap **one origin from the head** for both eyes; each eye exports its world position, that shared high/low origin, and float32 view/projection matrices relative to the origin. These matrices must only consume origin-relative positions.
- Keep existing mono `RenderView` and 256-byte HLSL ABI unchanged. This helper is for callers migrating scene view/culling/raster to a shared relative coordinate contract, not a replacement for the existing absolute-world view allocator.

## Rendering integration still required

1. OpenXR session, action poses, swapchain images, runtime per-eye FOV / projection, and predicted display time.
2. Stereo views in the render graph, using a shared high/low origin with per-eye relative VP for GPU scene raster and culling; a two-eye target or array-layer path.
3. Per-eye depth, HZB, visibility, transparent passes, postprocessing and histories. Treat the two-eye visibility union conservatively if sharing draw/cull lists.
4. Per-eye temporal state, motion vectors, jitter and reprojection. A head-origin change must invalidate or correctly rebase histories for **both** eyes together.
5. VR frame pacing, late pose updates where supported, comfort-oriented graphics settings, and headset/device testing.

## Validation

`DoubleFloatTests` now covers IPD survival at +/-1e12 units, cell-edge rebasing, a shared split origin, head-local rotation and independent eye orientations. GPU/device/OpenXR verification is not yet included.
