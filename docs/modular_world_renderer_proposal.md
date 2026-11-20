# Proposal: modular world rendering and selectable geometry backends

Status: **scaffold only**. No active render path, shader, rendering behavior,
resource ownership or existing `IRenderer` ABI is changed by this PR.

## Goal

Separate (1) world/scene lifecycle, (2) streaming and GPU residency,
(3) scene geometry publication and visibility, and (4) frame composition.
Allow a WorldRenderer instance to select CPU-recorded indirect, GPU-scene
indexed indirect, GPU-scene task/mesh, or restored GeometryFX filtering.
Share lighting, material families, shadows, depth/Hi-Z and post-processing,
without reloading or re-registering the world for each backend.

## Existing seams

- `engine::IRenderer` is mainly an identity + 256-byte push constant ABI,
  not a full render pipeline interface. Preserve layout and the shader mirrors.
- `smvk::IFrameRenderer` owns persistent pass resources and appends passes.
  Do not conflate that lifecycle with selection of a geometry submission path.
- `WorldRendererElement` already separates camera/input/application control.
- `WorldRendererConfig::FilterOnly` is a *pass policy*, NOT a resurrected
  filter implementation.
- `GfxRenderer::AppendPasses` is currently a no-op. The GeometryFX
  filter/seed/remainder shader chain was retired.
- `tour::gpu_scene::SelectRasterRoute` establishes exclusive raster
  ownership; preserve admission, explicit fallbacks and material distinctions.
- `tour::visibility` already provides renderer-neutral Hi-Z structures.

## Target ownership

```
App / Editor
  └─ WorldRenderer (IRenderer: composition only)
      ├─ SceneSource / SceneFrameSnapshot (owned or borrowed World)
      ├─ StreamingCoordinator (geometry+texture residency and feedback)
      ├─ GeometryRepository (canonical IDs, upload, per-flight publication)
      ├─ GeometryBackend (selectable geometry visibility/submission)
      │   ├─ CpuIndirect
      │   ├─ GpuSceneIndexed
      │   ├─ GpuSceneTaskMesh
      │   └─ LegacyGfxFilter
      └─ shared rendergraph phases (depth/Hi-Z, shadows, lighting, post)
```

The scene source is authoritative for world identity, transform and edit
revisions. GeometryRepository is authoritative for persistent geometry IDs,
material access and physical residency. Backends own only their private
culling/work/indirect buffers and pipelines, not duplicate worlds or meshes.
The frame composer owns render targets, pass dependencies and scheduling.
State may be reused across backend switches only after frame-flight safety
is established.

## Contract introduced in this PR

`engine/include/Engine/Renderer/GeometryBackend.h` defines a small
CPU-only configuration, structural validation, backend enumeration and
forward-declared pass API. It intentionally does NOT add a new implementation,
select a backend at runtime, or change `WorldRenderer`/CMake shader build.
The opaque frame input and recorder must be made concrete with explicit
resource and frame-flight lifetime contracts as part of the next PR.

A backend must declare depth, masked and opaque submission separately.
The *composer* chooses the scheduling graph; it must never assume
a universal linear depth -> Hi-Z -> shading sequence: static occluder
depth, skinned bounds/skin dispatch, late depth, per-view shadow visibility
and compute-to-indirect barriers need explicit dependencies.

CPU-recorded indirect means CPU-generated/uploaded Vulkan indirect commands;
it does not imply software rasterization. Indexed GPU-scene uses GPU-produced
draw commands. Task/mesh and legacy filter use the same canonical geometry
identity and must not duplicate draws across ownership routes.

## Retired GeometryFX restoration

The author has confirmed that the old branch and commits are available,
including shader recovery. Restoration is therefore a **port**, not a
from-scratch shader rewrite. Before wiring an implementation:

1. Locate the exact revision containing working
   `gfxfilter.cs.hlsl`, `gfxfiltered.vert.hlsl`,
   `gfxbatchcull.cs.hlsl` and matching C++ filter/seed/remainder logic;
   record commit hashes and baseline frame captures.
2. Recover source into a separate experimental backend, verify HLSL/C++
   push-constant, material, draw-record, cluster and shader-varying ABIs
   against current Tourmaline; do **not** overwrite current GPU-scene shaders.
3. Adapt to canonical geometry registration and shared Hi-Z. Preserve
   per-view visibility, mask/double-sided behavior, skinned/direct spill,
   temporal seed correctness and depth-conservative occlusion.
4. Gate activation behind explicit capabilities and a disabled-by-default
   selector until capture parity and Vulkan validation pass.

The exact historical revision is deliberately left unpinned here: it must
be selected by inspecting the user's surviving branch/commit history.

## Proposed incremental PR sequence

1. **Scene snapshot boundary:** extract source/borrowed World handling,
   transform/material revisions and editor mutation synchronization from
   `WorldRenderer.h/.cpp`. Preserve existing public setters as adapters.
2. **Streaming boundary:** extract `UpdateGeometryStreaming`,
   `UpdateTextureStreaming`, demand feedback and residency updates;
   explicit per-flight ownership, no GPU readback stalls.
3. **Canonical geometry boundary:** publish instance/mesh/material tables
   once per acquired flight; keep persistent IDs, LOD and fallback routing.
4. **GPU-scene backend adapter:** wrap the already-working indexed path
   using the new contract; compare identical output to master.
5. **CPU indirect backend:** generate per-material indirect command groups,
   preserving opaque/masked semantics and current shader variants.
6. **Shared depth/visibility orchestration:** extract prepass/Hi-Z and
   per-view dependency setup without breaking skinning or shadow passes.
7. **Legacy filter backend port:** recover source from historical commits,
   adapt data ABIs, validate shader and draw parity.
8. **Runtime selection and editor/UI:** explicit backend capability and
   failure handling; switch only at a GPU-flight-safe boundary.

## Acceptance gates

- This PR: header contract has no Vulkan dependency, CPU contract test
  registered with CTest, no change to existing rendering/ABI.
- Each implementation PR: native Windows/Linux compile, RendergraphContract
  tests, Vulkan validation-layer smoke, main + shadow + masked + skinned
  scene captures, pipeline timing and GPU memory/streaming regression.
- No duplicate raster ownership, missing masked depth pixels, stale
  indirect buffer reads, invalid cross-flight references or silently
  ignored requested backends.
- Explicit fallback or an error if a backend lacks a required shader,
  feature or material route; never silently pretend legacy GFX is active.

## Rough planning estimates

Scene 2–4 days; streaming 3–6; geometry 4–8; backend contract/adaptation
3–6; CPU indirect 3–7; pass sharing 4–8; historical GeometryFX port
4–10 (sources available, integration/parity still required); validation
5–10. These are developer-day planning ranges, not measured results.

## Related

- `docs/gfx_retirement.md`
- `docs/audit-prs/10-world-renderer-decomposition.md`
- `engine/include/Engine/Renderer/gpu_scene_render_route.h`
- `engine/include/Engine/Renderer/visibility_hiz.h`
- `engine/include/Engine/Renderer/WorldRendererConfig.h`
