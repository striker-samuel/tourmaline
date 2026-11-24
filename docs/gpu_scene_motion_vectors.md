# GPU Scene motion vectors — Phase 0 audit

Read-only audit of the current motion-vector + skinning pipeline, before any
code changes. Findings below.

## Skinning / deformed heap

- **Deformed heap layout**: 5 words per vertex (20 B) — `[0..2]` position
  (float3), `[3]` oct-encoded normal, `[4]` oct-encoded tangent. Written by
  `gfxskinning.cs.hlsl`, read by the draw paths via `(vertexId - deformedBase) * 5`.
- **Parity double buffer**: `WorldPage::EnsureSkinnedGpuData` allocates the
  deformed buffer as two halves. Each frame the skinning pass writes the *cur*
  half (`push.deformed = heapBda + (parity ? span : 0)`), leaving the other half
  as the previous frame's pose. `m_skinHeapParity` flips per frame
  (`WorldRenderer.cpp` ~15373).
- **Previous deformation is ALREADY retained** in `smvk::gfx::State`:
  - `skinDeformedBda` / `skinDeformedBase` — current half.
  - `skinDeformedPrevBda` — previous half (captured at ~15314, before the new write).
  - `skinPrevValid` — 1 only while consecutive frames ran skinning on the
    SAME heap + span + base (`lastSkinHeapBda == skinHeapBda &&
    lastSkinDeformedBase == push.deformedBase`, ~15408).
- **Identical addressing**: cur and prev halves share the SAME `deformedBase`
  and vertex ordering (the parity flip only changes the half offset).
- **`vsPrevDeformed`** (`gfx_types.h` ~858) is the direct-path VS prev tail,
  also already present.
- **Gap**: `FrameContext` carries only `skinDeformedBda` / `skinDeformedBase` /
  `skinDeformedValid` (current). The GPU-scene motion push constants
  (`GsTaskMeshPush`, `RasterMotionVsPush`) carry only the current `skinDeformed`
  + `skinValid`. The previous half is NOT yet propagated to the GPU-scene
  motion shaders.

## The skeletal-motion bug

- `gpu_scene_meshlet_motion.mesh.hlsl` (~126): fetches the CURRENT deformed
  position into `local`, then uses `local` for BOTH current (`relative`) and
  previous (`prevRelative`) projections. Skeletal deformation is omitted from
  velocity; a limb moving relative to the root reads as zero mesh motion.
- `gpu_scene_motion.vert.hlsl` (indexed path): uses `IN.position` (bind-pose
  vertex buffer) for both frames and never fetches the deformed heap at all —
  unlike `gpu_scene_raster.vert.hlsl` which does fetch `skinDeformed` for the
  current frame. The indexed motion path therefore ignores skinning entirely.

## Velocity encoding

- `gpu_scene_motion.frag.hlsl` (~62): `motion = prevUV - currUV`, then
  `if (dot(motion,motion) > 1.0) motion = 0`. This magnitude heuristic both
  discards legitimate large motion and fails to reject invalid `prevClip.w`
  (division by non-positive W) / non-finite values.
- Convention: RG16F, `prevUV - currUV`. Consumers (DLSS/SMAA/SSR/SSGI/motion
  blur) consume this; jitter policy must be documented per consumer.

## Motion pass orchestration

- **GPU-scene motion is OFF by default** (`TOURMALINE_GPU_SCENE_MOTION=1`
  re-enables). It still device-losts (a missing barrier on the shared raster
  streams) — the direct per-node path is the current fallback.
- The motion pass (`MotionVectors`, `motionvector.h`) re-rasterizes geometry
  read-only against the 1x depth prepass.
- The batched task/mesh leg dispatches `DrawMeshTasksIndirect` over the SAME
  task/mesh indirect + group/offset/work streams (reuses the task shader's
  culling output — no re-culling) but runs the motion MESH shader for every
  surviving cluster, **including static geometry** (Phase 3 target: eliminate
  this static replay).
- The indexed leg re-runs the raster VS for all admitted raster work.

## Temporal history

- Previous view + instance transforms (`prevView`, `prevTransforms`,
  `prevValid`, `prevSlots`) are published into the task/mesh params earlier in
  the frame (`TaskMeshInputs` fill). Generation-validated per slot.
- `prevValid == 0` (first frame / camera cut) -> `prevClip = currClip` (zero motion).
- Instance generation alone does NOT validate the skinned allocation; the
  deformed half's validity (`skinPrevValid`) is the missing piece.

## Consumer list (to confirm exact jitter/resolution/sign conventions)

- DLSS, SMAA T2x resolve, SSR temporal accumulation, SSGI/other temporal
  lighting, motion blur. `motionvector.h` pins `prevUV - currUV` RG16F.

## Motion generation when unused

- The motion pass is currently added unconditionally (no consumer gate). Phase 5
  requires skipping it when no consumer needs it.

## ABI surface to extend

- `TaskMeshGpuPush` (C++ `gpu_scene_task_mesh_abi.h`) + `GsTaskMeshPush`
  (HLSL `gpu_scene_task_mesh.hlsli`): add `skinDeformedPrev` (u64) +
  `skinPrevValid` (u32).
- `RasterMotionVsPush` (HLSL `gpu_scene_motion.vert.hlsl`) + its C++ mirror:
  add current `skinDeformed`/`skinDeformedBase`/`skinValid` AND
  `skinDeformedPrev`/`skinPrevValid` (indexed motion currently has none of
  these).
- `FrameContext`: add `skinDeformedPrevBda` + `skinPrevValid`.
