# Distant-view gfxfilter color-kill: investigation and validation

Status: **unresolved rendering regression**. This branch hardens the filtered-index ring
bounds check and adds coarse diagnosis counters; it does **not** claim that the far
view now renders. Starting evidence: commit `bd83e1ed`.

## Reproduce

- Use the packed grid cameras in `benchmarks/scenarios.json`: grid
  `(0,60,-110)`, grid32 `(0,300,-600)`; compare against a near working view.
- GPU draw verdicts reported `3789/4096` visible, but sampled main-view
  indirect commands read `indexCount=0, instanceCount=1` at the far camera.
- Mesh main gates each batch on that index count in
  `engine/shaders/gfxfilter.mesh.hlsl`; indexed main uses the same indirect buffer.
- Seed depth uses separate seed commands and hardware rasterization, so visible
  depth does not prove main-filter triangle survival.
- Existing main indirect readbacks were sampled during recording: their timing
  and their coverage of only the first few slots must not be treated as a
  synchronized, exhaustive proof of the final buffer state.
- The prior claim that *all* near-view indexed args are zero is unverified by
  the small readback sample; the near mesh shader requires nonzero batch commands
  to render. Keep the indexed leg as a separate regression to test.

## Changes in this branch

- In the filter, use subtraction-safe capacity testing
  (`outBase <= GFX_MAX_FILTERED - gLocalCount`) so a wrapped unsigned
  sum cannot authorize an out-of-bounds filtered-index write.
- Count filter workgroups by mutually exclusive outcome:
  `gated` (batch/draw verdict or cluster occlusion),
  `empty` (no triangle survived), `overflow` (positive count but no
  filtered space), `allocatedBatches` (positive count and space).
- `allocatedIndices` is the raw atomic reservation counter. It **includes**
  overflowed reservations, so it may exceed the capacity; do not call it
  the number of usable indices.
- `DumpIndirectAudit` prints these counters from GPU cullState. Use it only
  after the filter dispatch completes for the relevant flight slice. Sampling
  while GPU writes are pending can produce stale or misleading results.
- Diagnostic atomics are temporary and incur cost on every candidate batch.
  Remove them (and resize cullState back) after isolating and fixing the cause.

## Investigation sequence

1. For *one settled near frame* and *one settled far frame*, synchronize after
   `GfxFilter` and read all `batchCount` indirect commands, not just the
   first three. Record nonzero command count, sum(indexCount), max(indexCount),
   and counts per indexed material range. Read the cullState counter and
   four outcome counters for the **same frame and slice**. Outcome categories
   must sum to batchCount; if not, first inspect pass scheduling, counter reset,
   synchronization and GPU readback timing.
2. If `overflow>0`, check ring usage, candidate counts and simultaneous
   overflow reservations. The filtered ring is sized for 65,536 batches of
   128 triangles; excess allocation currently drops batches. Distinguish a
   genuine capacity limit from counter corruption.
3. If `empty` dominates, run a diagnostic build that temporarily **keeps
   triangles** but retains the verdict gates and capacity limit. Do not ship
   this bypass. If geometry returns, instrument homogeneous clip rejection
   independently: all-behind `w`, left/right/bottom/top guard bands, and
   `z<0` / `z>w`. Count overlaps separately. Also confirm backface and
   small-primitive flags remain disabled for isolation.
4. Capture the complete compute `FilterPush.viewProj`, actual output view
   matrix, input vertex positions, model transforms and representative clip
   vectors for the same world triangles at near and far cameras. The C++
   `BeginView` path copies `RenderView.m_viewProjectionMatrix`; do not
   assume it equals the live output matrix under cull-freeze or jitter.
   Clip-space Z in Vulkan zero-to-one is `0 <= z <= w` (also with reversed
   depth); flipping the near/far labels is not a fix.
5. With `TOURMALINE_MESH=0` compare nonzero indexed commands against the
   actual indexed draw ranges, vertex input, instanceID / firstInstance and
   raster state. For static geometry, indexed and mesh should both draw.
   For skinned geometry, note that the compute filter currently uses
   static positions while the mesh/VS paths fetch deformed positions.
6. Re-run packed grid and grid32 visually **before** trusting the historic
   79.9/95.6/320 ms sky-plus-sliver benchmark figures. Verify draw-cap
   behavior separately: kMaxDraws=4096 limits the packed overview; instancing
   is the structural solution, not a triangle-cull fix.

## Acceptance checklist

- [ ] Reproduced the far-view failure on Vulkan hardware with matching GPU evidence
- [ ] Identified whether the failure was empty/gated/overflow or stale readback
- [ ] Explained any per-plane rejection with clip-coordinate evidence
- [ ] Far and near views show the intended geometry with mesh shaders on
- [ ] Near and far views show the intended geometry with mesh shaders off
- [ ] No filtered-index out-of-bounds writes or unexpected allocation overflow
- [ ] Removed temporary diagnostic counters/readbacks after fix validation
- [ ] Re-ran visual and frame-time benchmarks with non-sky geometry

## Scope and caveats

The unsigned-bound hardening is a real safety improvement, but it does not
establish the root cause of the distant-view sky-only frame. No GPU execution
or local build is claimed for this branch. The draw-cap/instancing effort is
out of scope.
