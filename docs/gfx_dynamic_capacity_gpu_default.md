# GfxFilter dynamic capacities and GPU-scene default

## Default GPU-scene raster

GPU-scene Main/depth is **enabled by default** for the canonical
source-admitted static opaque subset delivered by #276. The existing
legacy GfxFilter and GPU-skinned pipeline handles all incompatible
objects. `TOURMALINE_GPU_SCENE_RASTER=0` explicitly opts out for
debugging/A-B parity. GPU-scene validation mode remains separate.
No other per-pass admission, shadow, material or motion-vector behavior
is changed in this stage.

## Legacy dynamic-capacity contract

The old CPU candidate fatal is triggered by a hardcoded 262,144
`kMaxBatchSlots` workload and the 65,535 `kMaxDraws` draw limit;
survivors have a separate `kMaxBatches=65536` fixed index-monoblock
budget. The newly introduced pure planning helper uses 64 KiB entry-
aligned pages and explicit overflow rejection, independently for draws
and candidates. This is **planning only**, not the active GfxFilter
allocator. Do not remove the legacy fatal checks until all consumers
are converted together.

### Required coordinated cutover

1. Compute exact draw/candidate counts before per-flight allocation;
   resize View draw/batch, indirect, kept parity, cullState and all
   seed/remainder, shadow, mesh-batch-list and debug buffers together.
   Retire old physical allocations safely after flight fences.
2. Rewrite HLSL fixed arrays/offsets in gfxfilter.hlsli, gfxbatchcull
   and the other compute kernels to use per-flight capacity fields.
   CullState counters must move to the runtime draw-capacity suffix.
3. Rebase GPU skinning bounds: deformed cluster capacity and per-draw
   union-bound tail must use exactly the same candidate/draw capacity
   as the cull pipeline. Never regress animated silhouettes.
4. Remove reliance on one-workgroup-per-draw dispatch with
   `drawCount <= 65535`; use chunked or multidimensional dispatch,
   preserving all temporal seed/remainder and debug offsets.
5. Keep the filtered-index survivor ring separately budgeted at
   `kMaxBatches`, or grow its virtual index region independently
   with correct monoblock lifetime and physical memory budgeting.
6. Exercise old-limit exceedance, allocation failures and Vulkan
   validation. No silent drop or partial draw admitted.

## Later renderer work

These are independent of dynamic GfxFilter capacity and must be
reviewed separately:

- **Layered materials on GPU-scene raster.** Publish material layers
  and all required FS/Forward+/G-buffer variants with screenshot
  parity; until then layered objects use the legacy material pipeline.
- **Remove the per-flight owner-generation mask.** Replace it with
  canonical membership in GPU-scene instance rows / generation-stable
  compaction or source-slot journals. Maintain exactly-once Main/depth
  submissions, O(dirty) updates and fail-closed source retirement.
  Do not simply remove the shader mask test while the instance-cull
  candidate stream includes legacy-owned objects.

## Verification

The default change requires real GPU static-only and mixed skinned,
masked, layered, lightmapped and LOD screenshot parity, plus Vulkan
validation. The dynamic-capacity helper has headless boundary tests;
the full runtime allocator/HLSL migration remains open work.
