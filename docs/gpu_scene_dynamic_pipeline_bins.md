# Dynamic pipeline bins on top of pak precount (#365)

## Status

CPU registry, variable-class histogram, stable class scatter and headless
regressions are staged on this branch. **This does not enable a new live
GPUScene submission route.** The eight-class indexed indirect renderer,
ProjectLit selection, task/mesh routing and legacy skinned fallback remain
unchanged until their consumers are migrated and verified on Vulkan.

## Identity and ownership

A `PipelineKey` combines vertex shader identity, fragment shader identity,
coverage, cull mode and a `renderStateId` representing **all other**
pipeline-compatibility state: render pass, attachment formats, depth/stencil,
blend, topology, vertex layout, MSAA and specialization. Use revision-stable
shader identities (or versioned keys), not ephemeral raw Vulkan handles.

`PipelineRegistry::Intern` deduplicates keys to dense IDs. IDs are
**registry-generation local** and must not outlive the registry's publication.
A material or mesh stores a shader-program selection; the renderer owns real
PSOs and their fence-safe lifetime. Skinning alone is not a pixel shader bin;
it changes the vertex fetch path and therefore may change the full key.

## Pak registration and per-flight publication

At pak metadata enumeration, count every potential drawable combination of
LOD, surface, material/shader family and registration source. Feed the
resulting `RegisteredPipelineDraw` records to `PrecountPipelineBins`.
The histogram forms disjoint conservative bin capacities. A separate
`PackPipelineFlight` classifies the acquired flight's active CPU candidates;
it uses one caller-owned contiguous span and preserves order within each bin.
A failed preflight leaves the output and published layout unchanged.

The capacity covers the entire set of possible draws; the GPU's exact
visibility counts can be lower. Source changes, procedural registration and
runtime shader swaps must trigger an updated registry/capacity plan or use a
fully admitted fallback. The current C++ `PipelineFlightLayout` owns a
`std::vector` and is **not yet a pointer-free GPU ABI**; upload a validated
flat descriptor/table if the GPU is to consume it. For CPU-visible bin
publication, hold both data and plan alive until the frame flight retires.

## Required production migration (not done in this branch)

1. Wire pak cooker/loader's LOD-surface-material enumeration and revision
   invalidation to the dynamic pipeline registry and conservatively
   measured capacities, including streaming and procedural registrations.
2. Resolve actual Vulkan PSOs for each key and retain their programs,
   pipeline layouts and resources until referencing flights retire. Check
   shader-stage and attachment compatibility; reject unsupported paths.
3. Define a flat GPU ABI with `pipelineId`, reserved `offset` and
   `capacity`, count-buffer offset, source generation and all required
   work-slot identity. Separate per-flight capacity and GPU-exact count.
4. Teach the GPUScene work classifier, prefix and scatter stages to operate
   over the registered dynamic pipeline IDs. The existing eight-bin
   `gpu_scene_raster_work_drawbuild.cs.hlsl` and four-class meshlet
   compaction kernels have fixed class assumptions; merely swapping their
   allocation does not implement dynamic submission.
5. Submit one `DrawIndexedIndirectCount` per actual pipeline bin with
   synchronization from compute command and count writes to indirect draw
   reads; preserve exact `firstInstance = originalWorkSlot`.
6. Migrate depth/prepass, Main, shadows/cascades, G-buffer, motion,
   temporal seed/remainder, Hi-Z, history and task/mesh fallback without
   duplicating ownership or clipping any command.
7. If a source, shader program, registration count, generation, PSO or
   capacity fails, fall back for that *entire source/flight* before assigning
   ownership. Do not clamp counts and draw a partially admitted scene.

## Acceptance gates

The new `GpuScenePipelineRegistryTests` checks key deduplication,
shader/cull/coverage separation, mixed-class precounts, exact offsets,
stable scatter, capacity bounds, empty scenes and failure atomicity.

Before enabling in production: compile C++20 and HLSL on supported
platforms; validate reflected GPU ABI; run the renderer's headless tests;
Vulkan validation and fences with multiple flights; screenshot Main,
masked/A2C, ProjectLit, depth, motion, cascaded shadows, and Hi-Z parity;
test skinned and static draws, runtime shader reassignment, LOD switches,
pak stream-in/out and overflow fallback; compare GPU timings and command
counts to the current eight-class fixed-stride implementation.

The original eight-class `pak_linear_pso_bins.h` remains useful as a
compatibility baseline during staged consumer migration.
