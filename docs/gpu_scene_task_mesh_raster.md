# GPU Scene task/mesh raster path (draft, not wired)

## Goal

Replace the dense meshlet candidate scatter + four-class PSO command
compaction **for mesh-shader-eligible geometry** with task-stage meshlet
visibility and mesh-stage triangle emission. Retain the indexed indirect
renderer for fallback geometry and as a parity baseline.

This first change adds only a deterministic CPU reference for forming
task workgroups. It does **not** enable a graphics pipeline, new shader,
or Vulkan draw call. Do not enable this path before the GPU work producer,
task/mesh shaders, ABI checks, and per-pass parity are implemented.

## Proposed dispatch model

After GPU instance culling, LOD selection, and submesh work generation:

1. Resolve the per-flight geometry alias and meshlet range. Route absent,
   oversized, stale, unsupported or malformed registrations to indexed
   fallback; never turn incomplete geometry into an empty mesh task.
2. Partition supported per-work meshlet ranges into chunks of at most 32
   meshlets. Each chunk carries original **work slot**, first cluster,
   cluster count, and raster PSO class.
3. Produce a dense task-group stream separated into the existing four
   raster classes. A GPU producer must do this without a CPU per-instance
   readback. Use a count/prefix/scatter scheme over actual groups, not
   a fixed 1M x N-task reservation.
4. Dispatch task groups through device-supported
   DrawMeshTasksIndirectEXT dimensions. One task group reads up to 32
   clusters, validates persistent instance slot + generation and geometry
   residency, and runs conservative camera-relative sphere/frustum and
   optional cone culling. Its workgroup payload contains surviving cluster
   IDs and original work-slot identity.
5. EmitMeshTasks(survivors, 1, 1) and let each mesh workgroup decode one
   cluster, pull shared vertices/indices, transform into the relative view,
   and emit triangles with the existing fragment-shader varying ABI.
6. Keep masked/double-sided PSO distinctions and separate indexed fallback.
   Reuse the same visibility-view freeze semantics and ensure optional
   Hi-Z agrees with the current depth pyramid.

## Important constraints

- Task shader culling eliminates mesh shader invocations for rejected
  clusters, **not** the initial task-stage scheduling cost.
- A task shader does not dynamically create global Vulkan draw commands.
  The task-work dispatch still needs GPU-generated indirect args.
- A task group must never cross a material/PSO or instance work boundary.
- Mesh output limits are device-specific; split or fall back if a cluster
  cannot fit the configured mesh shader's vertex/primitive outputs.
- The existing gfxfilter.mesh.hlsl is a useful varying/vertex-pulling
  reference, but reads a different CPU-built GfxFilter batch ABI.
- The existing compact meshlet consumer currently has unresolved Sponza
  parity issues. Compare against both whole-submesh and indexed-compact
  paths and validate all four PSO classes, both depth and Main.
- Temporal seed/remainder and HZB history require an explicit correctness
  contract before routing ownership to this path.

## Initial reference tests

The CPU oracle in gpu_scene_task_mesh.h tests 32/33 boundaries,
class separation, original work-slot identity, whole-submesh fallback,
and all-or-nothing overflow. It deliberately does not claim that the
proposed GPU path is implemented or fast.
