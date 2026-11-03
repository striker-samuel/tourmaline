# CPU reference scene renderer (Embree)

The engine exposes `tour::renderer::ReferenceSceneRenderer` in
`<Engine/Renderer/ReferenceSceneRenderer.h>`. This is a reusable
GPU-independent camera renderer, **not an editor-only implementation**.
It traces against a finalized `GiTraceScene` (the same CPU scene
representation used by GI baking). The BVH is built using Embree, and
camera rendering uses the GI tracer's metallic-roughness BSDF / direct-light
evaluation with opt-in camera emission and smooth vertex normals.

## API and ownership

```cpp
#include <Engine/GI/bake.h>
#include <Engine/Renderer/ReferenceSceneRenderer.h>

tour::gi::GiTraceScene scene;
// Fill positions/indices/triMaterial/materials/normal data,
// or call ExtractTraceSceneFromWorld/Cluster with appropriate opts.
scene.SetTraceBackend(tour::gi::TraceBackend::Embree);
scene.Finalize();

tour::renderer::ReferenceCamera camera;
tour::renderer::ReferenceRenderSettings settings;
settings.samplesPerPixel = 64;
auto image = tour::renderer::ReferenceSceneRenderer::Render(scene, camera, settings);
auto rgba8 = image.ToRgba8(settings.exposure);
image.WritePfm("reference.pfm"); // linear HDR pixels, no tonemap
```

The caller owns the scene and keeps it immutable for the duration of
`Render`. The renderer does not touch Vulkan/RHI resources. Scene extraction
from a live World must happen on the owning thread; the expensive BVH
finalize and camera tracing can happen on a worker over the detached
snapshot. `ReferenceRenderSettings::threads` controls CPU workers;
seeds are per-pixel/per-sample so images are byte-deterministic across
thread counts. Optional atomics provide cancellation and rows-completed
progress. Cancellation returns an **empty** image instead of publishing an
incomplete one.

`ReferenceImage::linearRadiance` is HDR linear RGB. `ToRgba8(exposure)`
uses a fixed ACES-fit curve and sRGB transfer. `WritePfm` exports the
unexposed linear pixels for numerical comparisons. The caller writes PNG
bytes as needed.

## Editor

The **Reference** tab in the editor's right-hand Inspector renders the
current live World using the active viewport camera. It captures edits
without saving or cooking, uses the existing `GiSceneBuildOptions` extraction
with `retainVertexNormals=true` and `includeDynamicLights=true`, decodes
available source KTX textures through the shared GI baker decoder, and
builds/traces asynchronously. It displays the resulting image using the
existing asset thumbnail cache, with PNG/PFM exports under the project's
`exports/reference_scene.*`. Closing the scene or editor cancels and joins
the job.

The real-time viewport remains the comparison image; use the
reference image alongside it under matching camera/exposure.
The reference settings and output are session-local: no scene schema,
pak or runtime material format changes are needed.

## Material Lab (headless)

Build the usual editor/tools targets plus `ReferenceScene`:

```sh
cmake --build out/build/linux-Release --target ReferenceScene
python tools/material_lab_fixtures.py --project out/material-lab/source
python tools/material_lab.py reference \
  --exe out/build/linux-Release/ReferenceScene \
  --project out/material-lab/source \
  --environment studio,cornell,outdoor \
  --material dielectric_smooth,metal_smooth,copper_brushed \
  --resolution 256 --spp 64 --bounces 3 \
  --output out/material-lab/reference
```

The fixture generator writes a `*.refscene.json` snapshot alongside
each `*.gltf`. Both derive from the **same in-memory geometry and
materials**, and share the authored analytic sky/sun settings. The
Cornell-style fixture includes its point light. Reference images are
recorded as `reference.png` and `reference.pfm` within the same
`environment/material/lit` matrix used by the Vulkan lab. Each has
`reference.json` provenance, plus the run's
`reference-report.json` and `contact-sheet.png`. The `reference`
command does not need a GPU, an App executable or cooked pak.

The standalone executable also accepts hand-authored/externally exported
scene snapshots:

```sh
ReferenceScene --help
ReferenceScene --scene path/to/scene.refscene.json \
  --camera-pos 0,2,5 --camera-fwd 0,0,-1 \
  --width 512 --height 512 --spp 128 \
  --output out/reference.png
```

The schema uses `schema_version: 1`, world-space `positions`,
`indices`, `triangle_materials`, optional `normals` and `uvs`,
a `materials` list (baseColor, metallic, roughness and optional
emissive), and an optional `environment` (sun/sky/spots). This is
a simple interchange format; it does not change native `.tour`
or `.pak` authoring.

## Interpretation and limitations

A CPU reference is valuable for **relative** PBR investigation but not
absolute physical ground truth. It reuses Tourmaline's existing GI
transport conventions: GGX/Schlick metallic roughness, analytic sky,
sun and punctual light next-event estimation. It currently:

- supports static triangles, material factors, textured base color and
  metallic-roughness on the live editor snapshot, vertex-normal
  interpolation and emissive visibility
- does **not** reproduce tangent-space normal maps, authored/project
  shader hooks, skinning, refractive glass, baked photographic IBL,
  volumetric scattering, post-FX or GPU-specific shadow algorithms
- uses GI's approximate pass-through transmission; masked/transparent
  behavior does not yet constitute an unbiased material reference
- excludes explicit sun-disc sampling on camera misses, and the
  present GI direct-light estimator is not a fully general MIS
  path tracer for area emitters
- reports its own fixed tonemapping, rather than claiming to match
  the runtime's variable tonemap/exposure/AA stack

For automated comparisons, start with studio dielectrics and metallics,
lock the scene inputs and exposure, and evaluate **linear HDR** frames
when possible. Do not mark an existing Material Lab GPU baseline as
physically accurate just because it matches this approximation.

## Validation

```sh
python -m unittest discover -s tools/tests -p 'test_material_lab.py' -v
cmake --build out/build/linux-Release --target ReferenceScene ReferenceSceneTests
ctest --test-dir out/build/linux-Release -R reference_scene_tests --output-on-failure
```

GPU/editor integration still requires a Vulkan-capable workstation;
the engine/reference tool does not need a GPU for its own tracing.
