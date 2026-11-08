# Phase 2: canonical GPU/legacy raster ownership

## Status

The production **mixed static-opaque GPU + legacy raster handoff is now wired**
behind the unchanged `TOURMALINE_GPU_SCENE_RASTER=1` opt-in. This document
records the implementation contract and outstanding verification. The PR
must stay draft until Vulkan scene parity and its compiler/tests pass.

This is *Main and Forward+ depth/G-buffer ownership*, not GPU shadows,
motion-vector cutover, or full GPU-scene material/skinning parity. Shadow
casters and motion-vector rendering still use their existing paths. GPU
skinning is already supported in Tourmaline; skinned objects deliberately
remain on the GPU-skinned legacy Main/prepass path.

## Source identity and admission

Static renderable identities are cluster registration + cluster-local node
slot + generation (`SourceKey`), not transient merged `NodeId` or sorted
`World::drawItems` indices. `WorldSynchronizer::SourceForNode` provides
the mapping. Compatibility records live in AppArena and are republished
on scene/topology/geometry revisions; dirty global transforms use the
existing `WorldSynchronizer` journal. Duplicate/invalid source identities
fail the entire raster admission closed.

Only single-surface, static opaque, single-LOD, indexed-resident objects
with the supported default material path qualify. Skinned, masked,
transparent, layered, lightmapped, project-lit and dither-closeup subtrees
remain legacy-owned. Unknown conservative bounds alone do not disqualify
a physically resident mesh: GPU instance/leaf culling fails open in that
case. Unsupported objects stay on the original GfxFilter/direct paths.

## Exactly-once Main and depth

Every acquired flight builds an *immutable* GPU owner mask indexed by
persistent instance slot, holding either the exact live generation or 0
for legacy ownership. The GPU instance cull reads the additional BDA
only in raster mode and refuses non-matching generations **before**
visible verdict publication. Validation mode uses a disabled mask and
retains the full CPU/GPU oracle.

On the CPU, one `CanonicalRasterOwnership` snapshot supplies the
complementary decision to:

- GfxFilter's Main-view batch/temporal seed and remainder producer.
- The direct opaque Main fallback.
- The Forward+ legacy depth and thin G-buffer writer.

GPU indexed depth clears the target once (or Loads a GfxFilter seed).
Legacy depth then Loads both depth and G-buffer and draws only
legacy-owned objects. If the GPU depth variant is missing, legacy depth
draws the full scene rather than leaving an uninitialized gap. The
Northlight split is disabled while mixed GPU raster is active; legacy
filter compute can still cull its own subset.

All ownership commits happen **after** required GPU compute/raster
buffers, mask upload and RenderGraph dependencies are prepared. Any
failure leaves legacy ownership for every source and suppresses
unmasked GPU work. Empty coarse-frustum selections commit ownership
without creating GPU draws (no candidate is on-screen).

## Flight and memory lifetime

The mask is per acquired flight, not mutated while its GPU work is in
flight. Rows are allocated in FrameArena for immediate transfer into a
retire-safe ResourceManager Holder. GPU buffers are 64 KiB capacity-
page-rounded and go through #278's VMA-pool creation path. A resize
rebinds the RenderGraph's external logical buffer and retires the old
physical buffer through the engine's existing deferred-destroy path.
The compute pass declares an explicit read dependency on the mask.

The CPU compatibility snapshot is AppArena-backed and sorted in place
once per topology/geometry refresh. Its storage is not mutated between
publication and command recording. A new acquired frame starts legacy-
owned; only a successful GPU work preparation commits admission.

## Validation gates before merge

- [ ] Windows/MSVC and Linux/Clang compilation, shader cook and C++ tests
- [ ] GPU/CPU validator parity across multi-flight source retire/reuse
- [ ] Raster opt-in screenshot parity: static-only, then static + skinned,
      masked/transparent, layered/lightmap/project-lit and mixed LOD scenes
- [ ] Forward+ thin G-buffer parity, shadow map and motion-vector continuity
- [ ] Vulkan validation: no new VUIDs or device faults
- [ ] Large-scene frame-time, memory/command-budget and visual profiling

## Separate capacity work (not a Phase 2 blocker)

GitHub Issues are disabled for the repository. Track dynamic GfxFilter
`kMaxDraws=65535` and `kMaxBatchSlots=262144` as a follow-up: change
dependent buffers, shaders, skinning bounds and temporal arrays together
to per-flight growable BDA/count/capacity, with multidimensional/chunked
dispatch for device workgroup limits. The separate `kMaxBatches=65536`
filtered-index survivor ring requires its own capacity policy.
