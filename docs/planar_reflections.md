# Planar reflections

Tourmaline supports one opt-in planar receiver with an internal low-cost reflected view or an externally supplied HDR capture. Both use the same pre-tonemap composite after SSR. Default is disabled. No second WorldRenderer, duplicate forward+ lighting, shadow updates, SSR or post chain.

## Internal capture API

```cpp
smvk::PlanarReflectionSettings settings{};
settings.receiverPlane = glm::vec4(0, 1, 0, -waterY);
settings.receiverCenter = glm::vec3(0, waterY, 0);
settings.receiverRadius = 20.0f; // world-space circular bound; 0 = infinite
settings.resolutionDivisor = 2; // quarter of main pixel count; 4 = 1/16
settings.maxDistance = 80.0f;
settings.maxDraws = 512; // surface draw calls, not nodes
// Mesh capture always chooses the coarsest available cooked LOD.
settings.includeMasked = false;
settings.f0 = 0.04f; // water; 1 for mirror
renderer.SetPlanarReflectionSettings(settings);
// renderer.SetPlanarReflectionSettings(std::nullopt); // disable
```

The receiver sphere gates visibility and bounds the compositor. It is not an exact mesh or material mask: coplanar surfaces inside the circle are still receivers. Settings are runtime C++ API; scene serialization and editor controls are not included. A host-provided capture, if installed, takes precedence.

For a reproducible App/headless run without editing host code, set the startup pin:

```sh
TOURMALINE_PLANAR_REFLECTION=0,1,0,0,0,0,0,20,2,80,512,1
```

Values are normal XYZ, plane d, receiver center XYZ, radius, resolution divisor (2 or 4), maximum distance, draw cap and F0. Example: a mirror at y=0. The pin is read once through RunConfig and logged; invalid values are rejected. Unset for the baseline.

## Capture and performance

For a bounded receiver, clip its conservative enclosing square in homogeneous clip space, compute its projected NDC rectangle, and crop both the reflected projection and capture target to that rectangle. Geometry and terrain culling use the cropped frustum; the compositor uses the matching cropped, unbiased VP. This reduces both raster work and submitted geometry when the reflector is small or viewed at grazing angles. Target dimensions round up to 32-pixel buckets and include two capture texels of padding. Infinite receivers retain the full reflected frustum. This is a conservative projected rectangle, not a tight polygon portal or depth-occlusion cull; resizing across buckets can cause allocation churn and must be profiled.

The internal pass creates its own reduced-resolution HDR color and depth targets, appends a custom reflected RenderView before the view slab upload, reverses material culling and applies biased reversed-Z oblique clipping. The unbiased reflected VP is used for receiver sampling (identical projected XY/W). It traverses resident static render items independently of the main view, selects the coarsest available cooked LOD, sphere-culls against the receiver-cropped reflected frustum, rejects distant objects and caps submitted surface draws. It reuses the shared static vertex/index heaps and flight-local material/light/view publications. It does not consume or write main-view indirect commands, temporal culling or Hi-Z.

The dedicated capture shader uses base color and emissive textures, directional sun and constant material ambient. Layered materials choose their dominant layer. Opaque static geometry is the default; masked geometry is optional. Terrain is captured using the L3 far grid (L2 fallback), with the same cropped frustum and distance limit, cheap sun/sky shading, holes preserved, reversed culling and no heightfield raymarch/shadows. Terrain blocks count against the shared draw cap before mesh draws. Transparent, skinned, particles, GPU-only instances without direct render items and nonresident geometry are not captured. Uncovered pixels retain the existing SSR/IBL/main HDR result using coverage alpha. No independent sky draw is needed for that fallback. The fullscreen composite is still full resolution and the CPU traversal is O(resident items), even after the draw cap. The cap uses traversal order, not a nearest-object priority queue. The simplified radiance will differ from full PBR. No temporal reuse is implemented; a visible receiver updates each frame.

| Main geometry route | Internal capture |
| --- | --- |
| CPU indirect / direct | Separate direct reflected draw traversal |
| GPU scene indexed | Same direct traversal; does not read main GPU commands |
| GPU scene task/mesh | Same direct traversal; does not read main meshlet visibility |
| Compute-filtered | Same direct traversal; does not read main-view Hi-Z |
 
This deliberately shares one capture path across main backends rather than requiring four new culling implementations. Parity depends on every relevant resident mesh being directly drawable; validate streaming and GPU ownership with the matrix below. The capture does not change the main backend selection.

## External capture API

The host may still call `MakePlanarReflectionCamera(mainView, mainProjection, mainEye, receiverPlane, reflected)` and render into a sampled RGBA16F HDR image. Use `MakePlanarObliqueProjection(reflected.view, mainProjection, keepPlane, clippedProjection)` if appropriate. Orient keepPlane toward the retained scene (reflected eye on negative side). The oblique projection preserves XY/W but changes far clipping. Reverse winding, exclude the receiver and use independent view resources.

Fill `smvk::PlanarReflectionCapture` with physicalView (or logicalView in the renderer graph), bindlessIndex, matching viewProjection, receiverPlane, thickness, strength and f0. Optional receiverCenter/radius bounds the composite; coverageAlpha defaults false for compatibility. When true, the capture must contain premultiplied HDR RGB and coverage alpha. External image and descriptor lifetime remain the host's responsibility through all in-flight frames. SetPlanarReflectionCapture(std::nullopt) removes the external override.

## Integrator validation / merge criteria

The following are acceptance targets, not measured results. Run on the actual supported Vulkan device and record configuration, GPU, resolution, scene, backend and before/after commit.

1. **Build and ABI:** build App and PlanarReflectionTests, compile all shaders including planar_capture, planar_terrain and planar_reflection; run ctest --test-dir <build> -R planar_reflection_tests --output-on-failure. Validate SPIR-V push offsets (composite 144 bytes; direct capture scene ABI 256 bytes), shader locations and descriptor access. No warnings/errors from Vulkan validation, including synchronization and lifetime.
2. **Backend matrix:** force each supported runtime route (CPU/direct, GPU scene indexed, GPU scene task/mesh, compute filter). Record actual selected route, not requested route. Unsupported routes are marked N/A with reason. A shared resident static-mesh fixture must appear in the reflection under every supported route, including GPU-owned meshes, with no duplicate draw or main-view visibility dependency.
3. **Visual correctness:** horizontal, vertical and tilted translated receivers; off-main-screen objects visible in mirror; behind-plane geometry excluded; correct mirrored winding for one-/two-sided materials and negative model scale; no receiver recursion or neighboring coplanar object outside radius; correct depth occlusion and reversed-Z; textured and emissive objects; optional alpha-tested foliage. Background uses the original HDR result without black halos. Inspect against a high-resolution reference capture and with SSR on/off.
4. **Feature interactions:** test AA off/SMAA/DLSS where supported, MSAA on/off, motion blur, DOF, HDR exposure, debug views, resize and odd dimensions, shader reload, camera crossing plane, invalid plane, exhausted RenderView slots, settings off/on, external override and removal. With no configured capture, no new pass or resource allocation and unchanged baseline output. With finite receiver fully outside view, no capture/composite pass. Verify no stale image or mismatched VP on toggles.
5. **Resources and ownership:** capture targets are within configured maximum dimensions and shrink with projected receiver coverage, single sample and valid distinct views; no main depth/visibility overwrite or capture feedback. Run frames in flight, resize and hot reload repeatedly with synchronization validation. Check geometry residency and draw surfaces with each route; report any GPU-only geometry missing from the direct fallback.
6. **Performance:** use GPU timestamps or RenderDoc/Nsight for capture and composite separately, and CPU record timing. Test 1920x1080 with 2x and 4x divisors, baseline/feature off, visible and offscreen receiver, 100/512/2000 candidate surfaces. Target <=1 ms total GPU overhead and <=0.5 ms CPU record overhead for the representative 1080p half-resolution fixture on the team's target desktop GPU (proposed budget, not a guarantee). Report median and p95 over at least 300 warmed frames. Draws never exceed maxDraws; disabled/offscreen must emit zero capture work. If budgets fail, reduce distance, LOD, resolution or draw cap; do not claim speedup solely from pixel count.
7. **Receiver crop and terrain:** compare cropped and uncropped reference images for tiny, edge-on, partially offscreen, near-plane crossing and tilted receivers. Verify no missing reflected geometry at crop edges, targets shrink with angle, and candidate draw counts fall. Inspect lowest mesh LOD for every draw, terrain L3 (L2 fallback), holes/coastlines, reversed winding and shared draw cap. Check resize churn and main terrain LOD/shadow behavior remains unchanged.
8. **Merge gate:** attach actual timings, visual captures, backend matrix and validation logs. Fix failures or explicitly agree to scope exclusions before merge.

Build, GPU execution and timing have not been performed by the PR author in this environment; the integrator owns execution and acceptance.
