# RFC: GPU-resident fields and texture-driven mesh deformation

> **PROPOSAL ONLY — DO NOT MERGE.**
>
> This document is for architecture discussion. It adds **no runtime implementation**, does
> not make the proposed APIs callable, and should remain a draft PR. No C++, HLSL, build
> configuration, serialization, or shader ABI changes are part of this proposal.

## Motivation

Allow gameplay, GPU Placement/instancers, physics contact events, authored node graphs,
and custom render-target captures to **record changes into a persistent GPU texture**,
then use that texture to **deform the rendered mesh**. Deformable snow (footprints,
wheel tracks, imprints from instanced rocks) is the first example, not a snow-only engine
subsystem. Reuse the same mechanism later for sand, mud, bent vegetation, and local
procedural deformation.

## Master audit — 2026-11-24

Audited tree: [6708a89b](https://github.com/striker-samuel/tourmaline/tree/6708a89b55205359b360efe431355757fe993875). The original proposal diverged from master at 0ca3600f; the branch comparison returned 462 master-only commits. This is a source audit, not a build or GPU validation.

### Existing seams and decisions

- `Engine/WorldFields/WorldField.h`, `WorldFieldTypes.h`, `WorldFieldAtlas.h`, `WorldFieldGpu.h`, `WorldFieldProvider.h` and `world_field.hlsli`: reuse the world-space field vocabulary, decoded units/fallback, residency concepts and WorldPartition publication boundary. Existing storage is a persistent **3D** brick atlas (64 interior, 2 halo per side), designed for streamed cooked/procedural data. CpuUpdated and GpuGenerated modes are reserved; registration is append-only after first publication, unregister is pre-publication only. Do not treat these as mutable surface APIs or reuse the stable table in-place for concurrent GPU writes.
- `Engine/Renderer/GeometryBackend.h`, `GeometryBackendTypes.h`, `gpu_scene_render_route.h`, and `docs/gfx_retirement.md`: target the modular geometry backend and canonical GPU Scene static ownership; the composer owns common depth/Hi-Z/targets. ComputeFilter recovery is shader-only per the November 21 record, not a working prerequisite. Do not revive GfxRenderer or obsolete lightmap layouts.
- `Engine/Graph/graph.h`: typed document and consumer-defined semantics; reuse topology validation, not an arbitrary shader generator.
- `Engine/Placement/placement_gpu.h`, `placement_ir.h`, `placement_pass.h`: reuse producer-native field math and instance streams through an explicit adapter. Do not assume historical record strides or a legacy procedural publication API; validate current producer count, layout, lifetime and GPU dependency at integration.
- `Engine/Rendergraph/rendergraph.h`: external image/view/buffer registration is available. Import persistent field versions and declare actual sampled/storage accesses; do not hide field hazards behind bindless indices.
- `Engine/Renderer/renderview.h`: AllocateCustomView remains a frame-local view, not target ownership. #337 is still a separate draft CPU-only selection contract. Offline geometry capture atlas is not a runtime capture facility.
- `docs/shader_library_design.md`: ProjectLit surface hooks and userdata are not a vertex deformation contract. Do not overload material userdata or inherit historical ABI offsets without inspecting current layouts.

## Architecture: surface-field extension, not a parallel world system

1. **Mutable surface field storage** (working name `SurfaceField`, under WorldFields): a renderer-owned 2D sampled/storage image, generation-checked identity, explicit XZ/UV mapping, units, fallback, version and reset. Keep this storage and mutable handle registry separate from the existing static 3D atlas and its fixed registration table. Share common semantics and later WorldPartition provider integration, not an incompatible storage ABI. A local non-streamed R16F compression field is the first candidate; query capabilities and use R32F fallback if needed.
2. **GpuFieldGraph**: domain compiler over the existing typed document. Own texture writes and scheduling, with shared math utilities only where semantics match. MVP is a fixed stamp/accumulate graph; do not require editor or asset cooking for first rendering.
3. **MeshDeformationBinding**: per-renderable opt-in binding (material may opt into a compatible shader family, but cannot own unique spatial state). Resolve via canonical source identity and generation into immutable flight-visible data. Use a common HLSL position/normal evaluator across admitted raster routes, leaving static geometry heaps unchanged.

The first use case is a dense static snow overlay with bounded world-Y displacement; no collision mutation, general skinning or terrain LOD support in v1.

### Suggested graph node taxonomy (proposal)

| Node | Typed inputs | Output |
|---|---|---|
| InstanceSource | producer/batch selector + generation | instance stream |
| ContactSource | contact-event buffer | stamp stream |
| OptionalCapture | selected world nodes + camera | depth/mask image |
| ShapeStamp | stamp stream + footprint shape/strength | stamp field |
| TextureSample | image + UV mapping | scalar/vector field |
| FieldMath | scalar fields; Add/Min/Max/Mul/Falloff/Noise | scalar field |
| History | previous logically published field version (GPU dependency enforced) | image/field |
| Accumulate | history + new stamps + policy | next field |
| TextureOutput | evaluated field | persistent output field |

A `History` node is **not a same-frame graph cycle**. Previous-frame read and
current-frame write are distinct versioned resources. Keep the structural graph
acyclic; model history as an explicit feedback resource edge across frames.

### Proposed user-facing sketch (illustrative, NOT IMPLEMENTED)

```cpp
auto field = renderer.CreateSurfaceField({
    .format = SurfaceFieldFormat::R16F,
    .extent = {1024, 1024},
    .worldBounds = snowBounds,
    .history = SurfaceFieldHistory::Persistent
});
auto graph = renderer.LoadGpuFieldGraph(project::graphs::SnowImprints);
graph.BindInstanceSource("rocks", placementBatch);
graph.BindContactSource("footsteps", footContacts);
graph.BindOutput("heightDelta", field);
world[snowNode].SetDeformation({
    .field = field,
    .mode = MeshDeformationMode::WorldYHeight,
    .maxAbsDisplacement = 0.25f
});
```

Exact method names, type ownership, project asset representation and runtime graph
cooking are open design questions. Prefer renderer/RHI resource interfaces to game
code holding raw Vulkan objects, bindless indices or device addresses.

## Execution and lifetime contract

- Compile on graph revision; validate source types, output ownership, finite inputs, extent/format and bounded work. Structural DAG remains acyclic; history is an explicit inter-frame dependency.
- At the frame publication boundary resolve live source identities, field generations, mapping, reset epoch, and conservative bounds; freeze bindings before visibility/recording. Publish changes once per simulation/render frame, not once per view.
- Import persistent images as RenderGraph external resources. Explicitly declare previous sampled image, stamp scratch and next storage image, followed by shader reads at every consumer stage. Handle cross-frame queue ordering, layouts and transfer/graphics/compute synchronization; external registration alone is not a barrier.
- Logical previous means the version used by the preceding rendered frame, not whichever fence completed most recently. Track physical version last-reader/writer retirement across flights. Two images are sufficient only if reuse is proven safe by dependencies; otherwise use a bounded retirement-aware pool or explicit serialization. Keep current/previous mappings and reset epochs with the snapshot. On reset initialize history coherently and invalidate motion/temporal history as appropriate.
- Start with bounded CPU contact events and one writer per texel (gather or tiled gather). Compression in metres: next = clamp(max(previous, stamp), 0, maxDepth); geometry moves downward. Specify pixel-center mapping, zero outside domain, overlap determinism, finite/clamped values and explicit clear. No assumed floating-point atomic support. No production readback.
- Placement comes later: consume producer-native buffers and GPU counts only once producer work and publication are valid. Never reinterpret placement data as GPU Scene rows, use a view-culled list as the authoritative stamp source, or promote instances to World nodes. Track generation/epoch and retirement; reject stale inputs, and define repeated stamp/event semantics.
- Stamp source capture is optional; reject same-frame capture/deformation feedback unless explicitly versioned. No per-view duplicate updates.
- Use ResourceManager holders and deferred destruction; defer image, descriptor and binding reuse until all consumers retire. Failed allocation or missing variant must choose a coherent fallback before admission, never a partly deformed object.

## Geometry integration

Introduce an engine-owned shared position evaluator (proposed, not implemented), independent of ProjectLit fragment hooks. Start with world-aligned static overlays; use stable world coordinates consistent with GPU Scene position reconstruction, and test large coordinates.

Inventory active route capability at the pinned master before modifying ABIs: GPU Scene indexed and task/mesh, direct indexed, depth/thin G-buffer, directional/spot/point shadows, motion, reflection/custom views where enabled. Relevant entry points include gpu_scene_raster.vert.hlsl, gpu_scene_meshlet.mesh.hlsl, gpu_scene_meshlet_motion.mesh.hlsl, gpu_scene_motion.vert.hlsl, gpu_scene_shadow.vert.hlsl and default.vert.hlsl. Verify actual pipeline registration and shader sharing rather than relying on filenames alone. ComputeFilter is a future capability check, not an MVP dependency.

The composer should select one coherent deformation capability per renderable across all required passes, using existing canonical ownership. If an enabled route lacks support, use a fully compatible direct route where available or disable deformation consistently; never mix displaced color with undeformed depth/shadow. Keep common Hi-Z and pass ownership intact. Preserve material/pipeline identity unless a variant genuinely requires a different pipeline. Any new CPU/HLSL layout requires pinned offsets, sizes and shader compilation checks.

Expand conservative bounds **before** instance/LOD and meshlet culling; account for displacement in shadow caster selection, occlusion and normal-cone tests (disable unsupported fine culling for deformed objects). Use the same current displacement for all current-position passes and the previous rendered version plus previous transform/mapping for motion. Reconstruct normals from field derivatives with correct world texel spacing. Reset/teleport/first-frame handling must avoid false velocity. No static geometry heap rewrites or mesh registration duplication.

Scope v1 to dense snow overlay geometry. Coarse terrain cannot reproduce footprints by texture sampling alone; near/far terrain, skinned meshes, collision and arbitrary displacement remain future explicit integrations.

## Optional render-target input

Keep #337 independent. Custom views are frame-local allocations, not persistent images or selection APIs. Once runtime capture exists, import depth/mask outputs through RenderGraph and validate generation-aware selection; keep render selection separate from light linking. The offline geometry capture atlas is unrelated. Contact-stamp MVP must not depend on #337.

## Implementation sequence (separate PRs; this RFC remains draft)

| Slice | Deliverable | Gate |
|---|---|---|
| 0 | Headless surface descriptor, generation/version/retirement model and route capability contract; document ownership under WorldFields | Architecture tests; stale handles, bounds, reset, invalid mapping, simulated multi-flight reuse |
| 1 | Persistent local 2D field, external graph registration, fixed CPU stamps, deterministic GPU accumulation and explicit reset | CPU oracle vs GPU readback (tests only), format capability/failure, overlapping stamps, no-op frames, Vulkan validation |
| 2 | Dense static overlay and shared deformation evaluator with coherent supported direct/GPU Scene route admission | Main/depth/G-buffer/shadow/motion parity, normals, expanded bounds, first frame/reset; unsupported routes fail coherently |
| 3 | Placement-native GPU source adapter and bounded tiled stamp processing | Producer retirement/reuse, GPU count bounds, off-camera source behavior, deterministic overlap, no readback; compare CPU/GPU cost |
| 4 | Typed graph compiler, cooking and authoring for proven operations | Headless graph validation, history feedback rules, revision caching, invalid source rejection |
| 5 | Optional WorldPartition surface tiling, streaming and capture | Seam/halo tests, tile eviction and reload policy, capture selection and graph dependencies; no implicit persistence promise |

### Acceptance and performance

- Run existing WorldFields, GPU Scene, architecture and rendergraph contract tests plus new surface field tests; compile supported C++ and HLSL configurations.
- Vulkan validation and capture/readback oracle on actual hardware; report exact backend, pass and capability matrix, not blanket parity claims.
- Exercise multiple flights, delayed completion, descriptor reuse, resize, teardown, stream unload, producer retirement, field reset, camera teleport and missing resources.
- Test identical displacement in depth/color/shadows and current/previous motion, static and moving transforms, grazing silhouettes, fine-culling boundaries and large world coordinates.
- Measure bounded image allocation, stamp count/overflow, dispatch cost, frame time, GPU memory and retirement high-water mark. A 1024² R16F image costs 2 MiB before allocation overhead; budget versions and scratch separately. No production CPU readback or unbounded per-frame allocation.
- Disabled feature preserves existing layouts/paths unless an explicitly reviewed ABI change is required; no claimed runtime results in this RFC.

## Decisions still open

- Exact surface registry owner, public handle/API and how it shares WorldFields concepts without changing static atlas publication.
- Actual current producer publication seam and record format; inspect implementation at slice 3 rather than freezing historical ABI.
- Exact per-renderable GPU binding transport and route capability matrix; confirm against current renderer at slice 0/2.
- Format/filter support and physical version count under the renderer's submission model.
- Tile ownership, persistence across unload, collision and non-world-Y deformation are deferred.

## Review policy

**DO NOT MERGE THIS PR.** Keep it as a draft discussion artifact. All runtime implementation belongs in separately reviewed and validated PRs. This audit changes Markdown only; no compilation, GPU testing or runtime implementation is claimed.
