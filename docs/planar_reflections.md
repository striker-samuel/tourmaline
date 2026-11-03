# Planar reflection compositor (experimental)

Tourmaline now provides a **host-supplied planar capture** compositor for flat mirrors and water. This is a separate technique from the existing screen-space reflections (SSR): it can display objects outside the main camera's view **if the host renders them into the reflection capture**.

## Scope

The opt-in `smvk::PlanarReflections` builder owns a full-resolution HDR target and a fullscreen program. `engine::WorldRenderer` composites one externally produced reflection capture after SSR and before motion blur / DOF / exposure / tonemap. With no capture installed, the extra pass is completely dormant. The default renderer remains unchanged.

The compositor does **not** render the secondary scene itself, define a new `.tour` node, manage capture image lifetimes, or support multiple/recursive mirrors in the same frame. For a production authored-water workflow, those additions are follow-up work. The caller is responsible for delivering a reflected-camera image.

## Produce a capture

Use `smvk::MakePlanarReflectionCamera(mainView, mainProjection, mainEye, receiverPlane, reflected)`, where `receiverPlane` is `(normal.xyz, d)` and `dot(normal, point) + d == 0`. The normal can be unnormalized. The helper returns a reflected view, reflected VP, reflected eye, and a `reverseWinding` flag. Keep the original projection (including Vulkan Y flip / jitter) or use a consistent new one in both rendering and compositing.

Render the reflected scene into a **sampled RGBA16F HDR image**, with the reflected camera's VP. Critical capture requirements:

- **Clip geometry behind the receiver plane** (prefer a GPU clip plane or an oblique near plane, oriented toward the reflected scene). Do not include the receiver surface itself; that would create a recursive reflection.
- **Reverse face winding/culling** (or disable culling) because mirroring inverts handedness. Rebuild or override view-dependent per-frame uniforms for this secondary camera.
- Produce the capture **before** the main compositor reads it. Avoid read/write feedback and keep the image, its physical view, and its bindless descriptor alive until all in-flight graph work has finished.
- Use the same `ResourceManager` for bindless image indices. The view may be different in size from the main HDR target, but its VP and image orientation must match.

The main renderer has a single-capture API:

```cpp
smvk::PlanarReflectionCamera mirrorCamera{};
if (smvk::MakePlanarReflectionCamera(
        mainView, mainProjection, mainEye,
        glm::vec4(0.f, 1.f, 0.f, -waterY), mirrorCamera)) {
    // Host: render the world with mirrorCamera.viewProjection,
    // clip behind the plane, and reverse face culling.
    smvk::PlanarReflectionCapture capture{};
    capture.physicalView = reflectedHdrView; // host-owned sampled image
    capture.bindlessIndex = reflectedHdrBindlessIndex;
    capture.viewProjection = mirrorCamera.viewProjection;
    capture.receiverPlane = mirrorCamera.plane;
    capture.thickness = 0.05f;
    capture.strength = 1.0f;
    capture.f0 = 0.04f; // water/glass (~0.04); set 1.0f for a mirror
    renderer.SetPlanarReflectionCapture(capture);
}
```

`physicalView` is imported by `WorldRenderer` into its private rendergraph on first use, and re-used while its handle remains unchanged. Alternatively, supply an already registered `logicalView` directly. Call `SetPlanarReflectionCapture(std::nullopt)` to disable the feature. Do not invalidate or recycle the capture's physical view while a frame is in flight. If a host replaces views, ensure proper GPU retirement before destroying the old images.

## Composite behavior and limits

The shader reads the main frame's reversed-Z depth, unprojects to a world point through `FrameUniforms.invViewProj`, and tests the normalized receiver plane with `thickness` as its world-space tolerance. It projects that point through the reflected VP, rejects outside-frustum samples, then blends captured HDR with the post-SSR HDR using Schlick Fresnel (`f0`), `strength` and an edge tolerance fade. Sky pixels and geometry off the receiver plane keep their original HDR untouched.

Because receiver membership is inferred from **depth versus an infinite plane**, coplanar objects share the same reflection. To restrict an effect to a finite mirror or water patch, a future authored surface mask/stencil channel is required; this first version assumes the receiving plane is unambiguous in the view. There is no roughness convolution, multi-plane selection, secondary-view lighting setup, or automatic clipping.

## Validation

- CPU math regression: `cmake --build <ninja-build-dir> --target PlanarReflectionTests`, then `ctest --test-dir <ninja-build-dir> -R planar_reflection_tests --output-on-failure`.
- GPU: build `App` (compiles `planar_reflection.frag.hlsl` to SPIR-V), render a flat plane with a host-produced reflected HDR view, move the camera so the reflected geometry is off-screen, and compare the capture/composite.
- Confirm no capture means identical output; resize, toggle capture, change plane height, inspect reflected winding/clipping and background rejection, and test SSR/AA/DOF enabled and disabled.

A complete shader/GPU build and rendered-frame regression have **not** been performed for this change in the development environment.
