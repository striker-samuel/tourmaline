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

## Status of this draft

Completed on the PR branch:

- Brought in current master (including the new geometry alias contract).
- SceneRegistry incrementally accounts for each live source's raster
  classification during reconciliation, using stable cluster/node/generation
  identity; updates, reclassification, removals, and Reset are covered.
- Current GPU-scene publication has exactly one submesh per selected LOD.
  Therefore each potentially emitted instance contributes *one* conservative
  command to its PSO class, regardless of current geometry residency.
  The general per-LOD maxima helper remains available for future multi-
  submesh publication; the registry accounting **must be updated** before
  removing the one-submesh publication restriction.
- Budget overflow is surfaced in `SyncStats::psoBudgetOk`; an unsuccessful
  count update must prevent activation of the compact raster path.
- CPU region-layout helper checks the sum of all four capacities against
  the published GPU work ceiling without mutation on failure.
- Experimental wave-aggregated append shader resolves geometry through the
  same revision-local alias mapping as master, and has checked per-bin CAS
  reservations plus overflow flags.
- Host push-constant/counter ABI definitions and headless tests cover
  generation reuse, changes between classes, and capacity overflow.

**Not yet enabled:** the live renderer still uses the fixed four-stride
builder. The append shader is experimental and not submitted by Vulkan.

### Remaining integration work

1. Snapshot the CPU per-class capacity with each acquired/retired flight's
   scene database publication (a current registry count must not size an
   older in-flight scene buffer).
2. Allocate four disjoint regions and reset per-flight GPU counters;
   build/dispatch the append compute pipeline with the new alias/geometry
   descriptor addresses and correct compute→indirect graphics barriers.
3. Issue four `vkCmdDrawIndexedIndirectCount` calls with the current-frame
   GPU counts and class-specific graphics PSOs. Count-buffer usage,
   offsets, per-bin capacity and Vulkan synchronization must be validated.
4. Update temporal seed/history/Hi-Z/depth passes to traverse the
   compact class ranges; they currently iterate fixed 4× work slots.
5. Establish a fail-safe admission path for unsupported scene sources and
   CPU budget overflow. The GPU overflow diagnostic alone is **not** an
   acceptable runtime fallback: no draw may silently disappear when a bin
   exceeds its capacity, including in the same frame.
6. Compile the new HLSL shader through the project's DXC toolchain and run
   Vulkan validation, parity screenshots and memory/performance benchmarks.

Do not enable the append shader until 1–6 are complete, including a
same-frame no-drop guarantee. Current master remains runtime-compatible.

## Performance verification

Compare total GPU frame time, compute time, indirect draws executed,
bytes written and memory reserved for skewed (mostly opaque) and mixed
PSO scenes, with tile churn and multiple in-flight frames. A wave append
adds atomics and may lose to the old path at small draw counts.
