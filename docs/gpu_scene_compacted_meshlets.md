# GPU Scene compacted meshlets — draft implementation plan

## Why

Confirmed grid-overlook A/B: GPU Scene active in both arms; 1,751 admitted
statics, 0 legacy. Fixed-lane meshlets: 3,158,804 reserved indirect commands
and 21.93 ms median. Whole-submesh: 7,004 reserved commands and 2.51 ms
median. Fixed lanes multiplied every stage's work by unused meshlet slots.

This draft removes the **PSO × fixed meshlet lanes** intermediate layout.
It does **not** alter production drawing yet. Both old paths are unchanged;
this is the start of a replacement, and should stay draft until the complete
runtime cutover and A/B succeeds.

## New stages

1. Reuse current instance cull -> compact -> GPU LOD/submesh work producer.
   Each work row still contains the original GPU Scene instance
   slot/generation, geometry/material and one of four raster classes.
2. `gpu_scene_meshlet_candidate_count.cs.hlsl`: one thread per current GPU
   submesh work record. Resolve the per-flight alias, indexed descriptor,
   existing GfxFilter mesh range and produce either its cluster count
   (1..1024) or one whole-submesh fallback. Invalid work produces zero.
3. Reuse existing **three-stage** `gpu_scene_work_prefix_{block,groups,add}`
   shader scan over these counts, with candidateCount=workCapacity,
   to produce exact offsets. The result at `meshletOffsets[workCapacity]`
   is the GPU-generated candidate total. A conservative per-flight
   candidateCapacity must accommodate the entire total; count is bounded
   by 1,024 meshlets per work item to avoid uint32 scan overflow.
4. `gpu_scene_meshlet_candidate_scatter.cs.hlsl`: generate one candidate
   per actual cluster via upper-bound binary search over the work prefixes;
   validate the persistent instance generation and per-flight geometry
   aliases; perform conservative meshlet sphere/frustum and optional
   normal-cone tests. A rejected candidate stores class INVALID, rather
   than writing an empty indirect command. Candidates preserve original
   work-slot identity. Malformed cluster index metadata fail open to a
   complete indexed submesh to avoid silently losing geometry.
5. `gpu_scene_meshlet_pso_count.cs.hlsl`: count surviving candidate
   classes per 256-wide block, using #301's block-count layout.
6. Reuse **unchanged** `gpu_scene_pso_prefix_scan.cs.hlsl` with the
   meshlet-offset buffer as its input count source. Provide a snapshot's
   per-class binOffsets/binCapacities and fail on overflow.
7. `gpu_scene_meshlet_pso_scatter.cs.hlsl`: wave-prefix scatter surviving
   candidates into dense Vulkan indexed-indirect commands. For every
   command, `firstInstance = originalWorkSlot`, not the compacted index.
   No successful-path global append atomics.

## Exact runtime wiring required before enabling

- Per-flight candidate counts, offsets, group sums/bases, candidate records
  (32 bytes each), block totals/bases, PSO GPU class counts/overflow, and
  compact indirect buffer. Every allocation must respect the flight fence,
  geometry publication revision and the RenderGraph logical lifetime.
- Schedule count -> work-prefix block/groups/add -> candidate-scatter ->
  PSO count -> PSO scan -> PSO scatter. Declare barriers for every BDA
  dependency and compute -> DrawIndirect for both commands **and counts**.
- Dispatch candidate scattering based on the **actual GPU total**, via
  `DispatchIndirect` args produced from the new offsets; do not launch a
  fixed 1M-candidate dispatch every frame. #301 PSO counting must similarly
  avoid reading uninitialized block counters when GPU totals vary. Its scan
  currently accepts fixed blockCount, so either clear unused blocks before
  reading or extend the scan to accept the actual GPU block count.
- Expose `DrawIndexedIndirectCount` through the RHI with Vulkan
  `INDIRECT_BUFFER` usage, a valid count-buffer address/offset and exact
  graphics PSO class ranges. The existing fixed-stride four-bin loops are
  **not** a compatible consumer.
- Migrate temporal seed/remainder, depth occluder, Hi-Z and visibility
  history to compacted command counts. They must never scan a reserved
  candidate capacity. Preserve per-instance generation/history identity;
  for true meshlet temporal history, also add a stable cluster key.
- Enforce **no-partial-render overflow**. A GPU prefix overflow flag alone
  does not permit clamping and drawing. Use a conservatively proved
  capacity for the entire flight, or a separate complete whole-submesh
  command stream prepared in parallel for GPU-side selection. Because
  overflow detection is GPU-produced, a same-frame CPU-only fallback
  that waits on readback would stall; a mask/indirect dispatch gate is
  preferable. Until this is solved, do not route ownership to the compact
  stream.
- Preserve double-sided/masked PSO semantics, masked Hi-Z bypass,
  mixed-scene legacy ownership, LOD streaming, skinned fallback, and
  camera-relative conservative sphere/cone behavior.

## Verification and benchmarks

The new CPU oracle is
`GpuSceneMeshletPrefixTests` (`tests/test_gpu_scene_meshlet_prefix.cpp`):
empty work, invalid/oversized fallback, 4-PSO stable ordering, exact
capacity, full-scale 1M candidates and transactional failure.

Integrators must compile the new HLSL kernels, check Vulkan push ABI
reflection, run CTest, and validate draw visibility and temporal parity
before enabling the new path. Measure GPU timestamps, actual GPU class
counts, candidate count, rejected count, and command count. Repeat the
verified grid-overlook A/B with `rasterActive=1` in both arms.

**Not claimed here:** runtime dispatch integration, indirect-count draw
consumption, complete same-frame overflow fallback, GPU validation or
performance gain. These are explicit merge blockers.
