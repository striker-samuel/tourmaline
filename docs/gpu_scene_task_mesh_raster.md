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

## Remaining runtime integration

- The C++ mirror `gpu_scene_task_mesh_abi.h` now checks structure sizes
  and offsets. **SPIR-V reflection remains required**, as do per-flight
  BDA allocation/publication and render-graph dependencies.
- Route only validated static submeshes to task/mesh ownership; retain the
  complete indexed fallback for all others. An unsafe cluster must not
  be dropped mid-submesh.
- `CommandBuffer::DrawMeshTasksIndirect` now records
  `vkCmdDrawMeshTasksIndirectEXT` through the RHI. **WorldRenderer does
  not call it yet**: task+mesh graphics pipeline construction, bindings,
  per-class dispatch, and depth/fragment parity remain to be wired.
- Publish the task-dispatch x dimension in `GsTaskMeshPush.taskGridX`,
  and ensure dispatch dimensions do not exceed device limits. Four PSO
  bins currently traverse the same list and reject mismatched classes.
- Run DXC/SPIR-V compilation, CTest, and pixel parity on sample/Sponza
  including depth, masked/double-sided, view-freeze and temporal behavior.
  No runtime or GPU validation is claimed in this draft.

## Initial reference tests

The CPU oracle in gpu_scene_task_mesh.h tests 32/33/1,025/4,096 boundaries,
class separation, original work-slot identity, whole-submesh fallback,
and all-or-nothing overflow. It deliberately does not claim that the
proposed GPU path is implemented or fast.
