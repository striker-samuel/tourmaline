# GPU Scene task/mesh raster path (draft, not wired)

## Goal

Replace the dense meshlet candidate scatter + four-class PSO command
compaction **for mesh-shader-eligible geometry** with task-stage meshlet
visibility and mesh-stage triangle emission. Retain the indexed indirect
renderer for fallback geometry and as a parity baseline.

The branch now contains experimental GPU task-group count/scatter, indirect
mesh-task argument generation, task-stage visibility and mesh-stage vertex
pulling shaders, a Vulkan RHI indirect mesh-task command, checked C++ ABI
mirrors, plus a deterministic CPU workgroup oracle. **None of these
new shaders is wired into WorldRenderer or Vulkan draw submission yet.**
Do not enable task/mesh raster ownership before ABI validation, shader
compilation, RHI/pipeline integration, topology validation and raster parity.

## Proposed dispatch model

After GPU instance culling, LOD selection, and submesh work generation:

1. Resolve the per-flight geometry alias and meshlet range. Route absent,
   stale, unsupported or malformed registrations to indexed fallback;
   never turn incomplete geometry into an empty mesh task. A submesh with
   more than 1,024 meshlets is valid and is split across task groups.
2. Partition supported per-work meshlet ranges of arbitrary valid size
   into chunks of at most 32 meshlets. Each chunk carries original **work slot**, first cluster,
   cluster count, and raster PSO class.
3. Produce dense task-group records using
   `gpu_scene_task_group_count.cs.hlsl`, the existing three-stage work
   prefix scan, then `gpu_scene_task_group_scatter.cs.hlsl`. The initial
   GPU implementation has ONE shared task-group stream, not PSO-compacted
   ranges. It submits four raster-class passes and the task shader rejects
   groups whose class differs (correct but up to 4x task-stage overhead).
   Per-class task grouping is a follow-up optimization.
4. Generate four 12-byte `VkDrawMeshTasksIndirectCommandEXT` commands
   using `gpu_scene_task_dispatch.cs.hlsl`, with device-supported x/y
   dimensions and a matching flattened group ID in the task shader.
   One task group reads up to 32
   clusters, validates persistent instance slot + generation and geometry
   residency, and runs conservative camera-relative sphere/frustum and
   optional cone culling. Its workgroup payload contains surviving cluster
   IDs and original work-slot identity.
5. `DispatchMesh(survivors, 1, 1, payload)` in the HLSL amplification
   shader, then let each mesh workgroup decode one
   cluster, pull shared vertices/indices, transform into the relative view,
   and emit triangles with the existing fragment-shader varying ABI.
6. Keep masked/double-sided PSO distinctions and separate indexed fallback.
   Reuse the same visibility-view freeze semantics and ensure optional
   Hi-Z agrees with the current depth pyramid.

## Important constraints

- Task shader culling eliminates mesh shader invocations for rejected
  clusters, **not** the initial task-stage scheduling cost.
- A task shader does not dynamically create global Vulkan draw commands.
  The dedicated compute dispatch-args shader generates them beforehand.
- Local compaction uses a 32-lane shared-memory inclusive scan, which
  produces stable exclusive ranks without global atomic append. All
  lanes execute DispatchMesh uniformly.
- The mesh shader currently emits three vertices per triangle, matching
  the existing GfxFilter mesh-shader strategy. Indexed metadata/vertex IDs
  must be validated **before** assigning ownership (shader checks alone
  are not a complete fallback or bounds proof).
- A task group must never cross a material/PSO or instance work boundary.
- The old indexed-compact path's 1,024-meshlet-per-registration gate is
  not a limit for task work: a 4,096-meshlet submesh requires 128 task
  groups. Enforce a total task-group capacity and check 64-bit prefix
  arithmetic instead of dropping large valid registrations.
- Mesh output limits are device-specific; split or fall back if a cluster
  cannot fit the configured mesh shader's vertex/primitive outputs.
- The existing gfxfilter.mesh.hlsl is a useful varying/vertex-pulling
  reference, but reads a different CPU-built GfxFilter batch ABI.
- The existing compact meshlet consumer currently has unresolved Sponza
  parity issues. Compare against both whole-submesh and indexed-compact
  paths and validate all four PSO classes, both depth and Main.
- Temporal seed/remainder and HZB history require an explicit correctness
  contract before routing ownership to this path.

## Runtime integration (opt-in, validation delegated)

The runtime is now connected to GPUSceneWork's selected GPU work rows.
Set `TOURMALINE_GPU_SCENE_TASK_MESH=1` to opt in on a device supporting
VK_EXT_mesh_shader (both task and mesh features). Without this pin, the
original compact/indexed raster path remains the default. If any required
shader, meshlet metadata, heap address, device feature, GPU-scene ownership
gate, or capacity bound is missing, the renderer keeps indexed rasterization.

With the pin enabled, the complete planned path is:

1. Reuse GPU instance culling and resident-LOD/submesh work generation.
2. Validate the entire published GfxFilter mesh/cluster/index registration
   on the CPU before assigning task ownership. All admitted static mesh
   geometry must fit the 64-triangle-per-meshlet mesh stage; missing,
   noncontiguous, malformed or out-of-bounds streams retain indexed
   ownership for the **whole frame** (no partial/double draws).
3. Allocate per-flight GPU task counts, prefix offsets, block scratch,
   group descriptors, indirect arguments, and task/mesh parameter buffers.
   Check the worst-case `workCapacity * ceil(worstMeshlets / 32)` against
   the one-million-task-group budget; more than 1,024 meshlets in a
   submesh is legal when the **overall** task budget fits.
4. Run GPU task count -> three-stage prefix scan -> task group scatter ->
   GPU Vulkan indirect argument build in the existing raster-build
   compute pass. Rendergraph dependencies cover the group, offset and
   indirect outputs. No intermediate CPU readback.
5. The task shader validates persistent slot/generation, runs conservative
   sphere/frustum and optional cone visibility, performs group-wide
   atomics-free prefix compaction, and invokes `DispatchMesh` once per
   visible cluster. The mesh shader pulls the GfxFilter cluster-ordered
   global indices and static packed vertex/shading data.
6. Bind the task/mesh graphics PSOs (opaque/masked x back/two-sided)
   and issue `vkCmdDrawMeshTasksIndirectEXT` for the depth/G-buffer
   prepass and the Main pass. The original fragment-shader material
   interface is retained.
7. Disable indexed temporal seed/remainder when task raster owns the
   frame, because that path assumes indexed command buffers were built.
8. Release graph-owned task buffers on detach.

This remains **draft until the integrator completes validation**. No
DXC compilation, CTest, Vulkan validation layers, screenshots, raster
parity, or GPU performance measurements have been executed here.
The initial four PSO passes each traverse the shared task-group list,
with nonmatching classes rejected inside the task shader. This is
correctly partitioned but can waste up to 4x task-stage work; per-class
GPU task-list compaction is a future performance refinement.

### Integrator validation checklist

- Build the task/mesh HLSL and inspect the reflected HLSL/C++ BDA and
  push-constant layouts (192-byte scene head + 24-byte task/mesh tail).
- Run `gpu_scene_task_mesh_tests` and the standard renderer tests.
- Capture depth/G-buffer/Main pixel parity against the whole-submesh
  indexed reference, including opaque, masked, double-sided and
  alpha-to-coverage material variants.
- Exercise both 32- and 64-lane subgroup hardware; culling-freeze,
  camera-origin precision, large (1,025+/4,096) meshlet submeshes,
  mesh stream republish, and invalid LOD residency.
- Exercise complete task-capacity overflow and CPU admission failure:
  no missing geometry, stale indirect commands or duplicate indexed
  ownership.
- Capture Vulkan synchronization and task/mesh validation with GPU
  tools, then benchmark task-stage cost against compact indexed draws
  on the grid and Sponza scenes. The existing indexed-compact Sponza
  parity issue is independent of this implementation.

## Initial reference tests

The CPU oracle in gpu_scene_task_mesh.h tests 32/33/1,025/4,096 boundaries,
class separation, original work-slot identity, whole-submesh fallback,
and all-or-nothing overflow. It deliberately does not claim that the
proposed GPU path is implemented or fast.
