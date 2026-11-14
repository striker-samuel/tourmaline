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

### Existing seams, not a greenfield renderer

- `Engine/Graph/graph.h`: UI-free `tour::graph::Document` with typed ports,
  topological validation and consumer-owned semantics.
- `Engine/Placement/placement_ir.h` and `placement_gpu.h`: field instructions,
  GPU SSA evaluation and producer-owned GPU instance streams. Placement currently
  *produces instances*; it is **not** a general persistent-texture processing graph.
- GPU Scene + the procedural batch publication contract (PR #331): use stable
  producer snapshots, not one World node per GPU-generated instance. Check
  publication/retirement epochs and per-flight buffer lifetimes.
- `RenderViewSet::AllocateCustomView`: per-frame virtual view allocation, not
  a persistent target identity.
- RenderGraph: explicit compute/raster resource dependencies.
- `docs/shader_library_design.md`: ProjectLit hooks provide a shading seam;
  arbitrary vertex displacement is **not** currently supported and must
  be introduced separately, with parity across all raster paths.
- Draft PR #337: CPU-only render-target selection vocabulary, **not** offscreen
  image allocation. A capture-based stamp source is optional and needs subsequent
  implementation.

## Architecture: three independent primitives

1. **Persistent `GpuField`**: renderer-owned, generation-checked handle to an
   image with format, extent, world-to-UV mapping, history/reset/clear semantics,
   explicit read/write usage and lifetime. `R16F` displacement is a first
   candidate; `RG16F` or more channels are future options. Define what values
   mean in world units, and allow explicit unclamped or clamped ranges.
2. **`GpuFieldGraph`**: a domain-specific compiler using `tour::graph::Document`
   for typed topology. This is not arbitrary runtime HLSL or a redefinition of
   Placement's instance-generation IR. It may reuse mathematical expression
   operations/evaluation patterns but owns texture writes, dispatch scheduling,
   source typing and resource binding.
3. **`MeshDeformationBinding`**: opt-in shader-time displacement that samples
   a field. A material/mesh may reference a field and a mapping. The source
   geometry remains unchanged; there is no mutation of GPU Scene's static
   geometry heaps.

```text
GPU Placement snapshots ----\
Physics/game contact events --+--> Source buffers / optional capture
Node graph field expressions --/                  |
                                                v
                               Stamp raster / bounded compute
                                                |
                           Previous field --> Accumulate
                                                |
                              Persistent GPU deformation texture
                                     /                    \
                          Vertex/mesh paths          Surface shading
                            + depth/shadows        + normals/roughness
```

### Suggested graph node taxonomy (proposal)

| Node | Typed inputs | Output |
|---|---|---|
| InstanceSource | producer/batch selector + generation | instance stream |
| ContactSource | contact-event buffer | stamp stream |
| OptionalCapture | selected world nodes + camera | depth/mask image |
| ShapeStamp | stamp stream + footprint shape/strength | stamp field |
| TextureSample | image + UV mapping | scalar/vector field |
| FieldMath | scalar fields; Add/Min/Max/Mul/Falloff/Noise | scalar field |
| History | previous *completed* field version | image/field |
| Accumulate | history + new stamps + policy | next field |
| TextureOutput | evaluated field | persistent output field |

A `History` node is **not a same-frame graph cycle**. Previous-frame read and
current-frame write are distinct versioned resources. Keep the structural graph
acyclic; model history as an explicit feedback resource edge across frames.

### Proposed user-facing sketch (illustrative, NOT IMPLEMENTED)

```cpp
auto field = renderer.CreateGpuField({
    .format = GpuFieldFormat::R16F,
    .extent = {1024, 1024},
    .worldBounds = snowBounds,
    .history = GpuFieldHistory::Persistent
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

## Execution model

- Compile/validate graph only on graph revision, **not** each frame.
- Consume GPU Placement's immutable published producer batches (per-producer key,
  incarnation/version, format/bounds, residency); never reinterpret compact
  40-byte placement records as 128-byte GPU Scene records.
- For small numbers of contacts, upload bounded stamp/event data and perform GPU
  stamping. For many instances, keep transforms on GPU and prefer tiled binning
  or rasterized stamp accumulation; do not read back positions.
- Use a separate temporary stamp image when overlaps require deterministic,
  well-defined blending. Resolve into the persistent field in one compute pass.
  Define accumulation explicitly (e.g. compression:
  `next = clamp(max(previous, stamp), 0, maxDepth)`). Avoid assuming R16F
  atomic max is available across all target GPUs.
- Use ping-pong or equivalent versioning for history; no simultaneous unsynchronized
  read/write of the same image. RenderGraph must know all image reads/writes.
- Schedule the update **before** depth/shadow/main passes that sample the field;
  if GPU Scene culling needs deformed bounds, publish expanded bounds first.
- Defining world-space mapping is mandatory: origin, axes, dimensions, clamp/
  repeat behavior, field ownership per tile/region, and effect of scene transforms.

## Mesh deformation integration

Propose an explicit opt-in geometry hook rather than changing the existing
ProjectLit surface-only contract without review:

```hlsl
// Pseudocode signature — NOT implemented.
float3 ProjectDeformPosition(
    float3 positionWS, float2 fieldUV, uint bindingIndex);
```

The hook is sampled during vertex/mesh generation and **must** apply
consistently to:
- direct indexed vertex path;
- GPU Scene indexed and task/mesh variants;
- depth/prepass, shadows, reflection views, and motion vectors;
- geometry normals/tangents when deformation changes slopes.

Shading-only parallax is not geometry displacement. Deep footprints need a dense
snow overlay/mesh, adaptive geometry, or another high-resolution geometry source:
the existing coarse far-terrain representation cannot produce centimeter-scale
silhouettes merely by sampling a texture. Existing `terrain.vert.hlsl` operates
on near-terrain world-space vertices; `terrainfar.vert.hlsl` reconstructs heights
from its terrain atlas. Integrate each explicitly, or scope v1 to snow-overlay
meshes (recommended), rather than silently adding divergent behavior.

Require conservative displacement-expanded world bounds for CPU/GPU Scene culling,
LOD, shadows, and occlusion. Sample the **previous** field consistently for
previous-frame motion vectors, rather than copying only the current deformation.

## Relationship to render targets

The render target is only **one possible graph input**: optional orthographic
depth/mask capture of selected objects from the World. A low-cost footprint
may be generated from contact events without any offscreen camera. A persistent
`GpuField` is not a view/camera target: its own storage, versioning and
compute-write semantics are first class.

No dependency on merging PR #337 should be required for the contact-stamp MVP.
If capture support is added later, keep render-layer masks independent of
light-linking masks, and resolve included/excluded World handles with generations.

## MVP and staged implementation (future PRs, not this PR)

1. Resource contract and headless tests: handles, ownership, extent validation,
   field history, graph type validation, stale producer/source rejection.
2. One persistent single-channel GPU field, explicit reset, bounded CPU-sourced
   stamps, versioned accumulation pass and Vulkan capture/readback tests.
3. One opt-in high-density snow overlay with a world-Y displacement shader,
   depth/shadow/motion parity and expanded culling bounds.
4. Placement producer consumption with batch-version validation, GPU stamp
   generation and overlapping stamp tests; evaluate performance vs CPU events.
5. Graph asset cooking/editor authoring, optional selected-object capture source,
   tiling/streaming, serialization of modified regions, and general field math.

### Validation/acceptance gates

- Deterministic reference comparisons for overlapping and repeated stamps;
  no race-driven divergence, no stale-image reads.
- GPU Scene on/off screenshot parity in main/depth/shadow views, including
  indexed + task/mesh paths and motion during deformation.
- Destroy/recreate fields, node deletion/reuse, stream unload and GPU producer
  retirement while flights are in progress.
- No unbounded per-frame copies/allocations, no hidden GPU->CPU readback.
- Explicit world-space mapping and tile-boundary behavior.
- Disabled paths preserve current engine shading, binary layouts and perf.

## Questions to resolve before implementation

1. Should the field be bound per World node, per material instance, or both?
   How do shared meshes and multiple deformed instances resolve mapping?
2. What shader hook ABI is minimal for direct/GPU Scene paths, including
   previous-frame deformation and normals?
3. Which parts of Placement SSA can be shared without coupling the instancer
   to texture writes? Is a separate field IR with shared math utilities cleaner?
4. What is the authoritative GPU producer record format and published snapshot
   contract during PR #331's follow-ups?
5. Are world-aligned Y-displacement overlays enough for v1, or should support
   arbitrary mesh-local normals be designed immediately?
6. What persistence/streaming policy is required for deformed fields on
   unloaded world tiles?
7. Which backend features are required for format/storage operations and
   what is the Vulkan parity test matrix?

## Review policy

**DO NOT MERGE THIS PR.** Keep it as a draft discussion artifact. Any eventual
implementation should arrive in separate, small PRs with independent tests and
explicit code review. No runtime implementation or validation is claimed here.
