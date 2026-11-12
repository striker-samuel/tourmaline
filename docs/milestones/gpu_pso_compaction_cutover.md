# Milestone: production four-class GPU-scene PSO compaction

**Status:** planned runtime integration.
**Landed foundation:** #301 atomics-free count/scan/scatter kernels, CPU oracle and device-free regression tests. #296 wave-atomic append proposal was superseded.

## Work remaining
- [ ] Budget and allocate per-flight class offsets, capacities, prefix buffers, compact commands, GPU counts and overflow flags for the exact acquired scene snapshot.
- [ ] Record count -> scan -> scatter with explicit BDA/indirect-command/indirect-count barriers and retirement safety.
- [ ] Wire four matching graphics PSOs through indirect-count submissions; preserve original work index/generation in firstInstance.
- [ ] Migrate seed/remainder, depth, Hi-Z and history consumers from fixed reserved slots to exact compact counts.
- [ ] Ensure GPU-reported overflow never silently drops draws: prove capacity or provide a complete same-frame fallback.
- [ ] Validate DXC ABI/reflection, native builds/CTest, Vulkan multi-flight parity, masked/two-sided LOD and streaming behavior.
- [ ] Benchmark image-correct fixed-bin vs compact streams; do not use sky-only or partial-scene captures.

## Acceptance
All four classes draw the complete intended visible set with matching mask and temporal behavior; overflow and failed publication fall back without lost geometry. Attach capture and frame-time evidence before enabling by default.

## References
- [Current TODO](../../TODO.md)
- [#301 prefix plan](../gpu_scene_pso_prefix_compaction.md)
- [Compacted meshlet integration notes](../gpu_scene_compacted_meshlets.md)

This draft records the implementation gates; it does **not** implement or enable the compaction path.