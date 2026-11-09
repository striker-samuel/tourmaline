# GPU-scene adaptive PSO bins (draft integration)

## Problem

The current `gpu_scene_raster_work_drawbuild.cs.hlsl` writes four
fixed-stride command regions. Each original work slot appears once in each
region and three of those four commands are normally zero. The temporal
and Hi-Z stages also iterate the expanded command capacity.

## Proposed ownership and layout

- CPU tracks conservative *per-instance* upper bounds of work emitted into
  each of the four existing PSO classes (opaque, double-sided, masked,
  masked-double-sided). It accounts for resident tile registration, removal,
  scene instance insertion/deletion, material changes, and changes to
  geometry/LOD eligibility.
- For each source, use per-class maxima over all GPU-selectable LODs; sum
  over sources. Do not count unique meshes, or just the current LOD.
- Update counts only when the matching GPU scene snapshot is published.
  Keep independent snapshot/buffer capacities for each in-flight slot.
- Compute four disjoint indirect regions using exclusive offsets of the
  upper bounds (plus optional headroom). Never change the size of an
  in-flight buffer until its fence retires.
- A GPU dispatch consumes GPU-created work records, aggregates each PSO's
  records at wave granularity, atomically reserves slots in that region and
  publishes dense `VkDrawIndexedIndirectCommand` entries.
- GPU-produced per-PSO counters are used by four
  `vkCmdDrawIndexedIndirectCount` calls (same-frame GPU counts). CPU
  bookkeeping sizes buffers; it does not decide how many draws are issued.
- Preserve original work index as `firstInstance`; the raster VS validates
  persistent instance slot and generation independently.
- Overflow must never write beyond any region or submit excess draw counts.
  Report overflows through GPU diagnostics and keep the existing legacy
  fallback for sources that cannot be conservatively accounted for.

## Staging / remaining integration work

This branch introduces a standalone conservative CPU bound helper, its
device-free test, and an opt-in GPU append shader. **It does not switch the
runtime to the new shader.** A subsequent renderer integration patch must:

1. Wire source lifecycle updates from World/streamed tile publication to
   the budget helper, including all submesh and material-class variants.
2. Extend the per-flight raster work allocation contract to expose
   per-class capacities/offsets and retain generation-safe snapshots.
3. Initialize four GPU counter entries each frame and bind the append
   shader rather than the fixed-stride draw builder.
4. Dispatch the append shader over the *GPU-produced* work capacity,
   preserving GPU work-count bounds and execution barriers.
5. Use GPU-side counter buffer offsets for four indirect-count submissions;
   ensure all four graphics PSOs are bound with the correct draw range.
6. Adapt temporal seed/history/Hi-Z kernels to the compact command layout
   and current per-class counts without reading stale commands.
7. Verify overflow signaling, work order, indexed geometry residency,
   Vulkan memory barriers, renderer parity and GPU frame-time measurements.

Do not enable the append shader before steps 1-7 are completed. The
original fixed-stride raster path remains unchanged in this draft.

## Performance verification

Compare total GPU frame time, compute time, indirect draws executed,
bytes written and memory reserved for skewed (mostly opaque) and mixed
PSO scenes, with tile churn and multiple in-flight frames. A wave append
adds atomics and may lose to the old path at small draw counts.
