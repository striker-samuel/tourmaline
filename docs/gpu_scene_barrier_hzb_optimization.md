# GPU-scene synchronization and HZB optimization

## Scope and status

This PR implements an **opt-in** first-stage Vulkan synchronization experiment. The existing global-barrier behavior remains the default. Further candidates below are design proposals, not implemented features or measured speedups. No GPU run has been performed for this PR.

## Findings

- [Vulkan barrier implementation](../engine/src/Vulkan/impl.cpp): EmitGlobalMemoryBarrier uses ALL_COMMANDS -> ALL_COMMANDS and MEMORY_WRITE -> MEMORY_READ | MEMORY_WRITE. This is intentionally conservative for BDA-only resources but overbroad between ordinary compute dispatches.
- [WorldRenderer](../engine/src/Renderer/WorldRenderer.cpp): RecordGpuSceneCullChain has 25 static global-barrier call sites across mutually exclusive branches; the ordinary parallel-scan indexed route executes 10 dispatches and 9 barriers, without optional task/mesh or indirect verification. RecordGpuSceneCascadeBuild has 9 additional compute edges per invocation. These counts exclude graph-managed dependencies.
- These barriers are GPU execution/memory dependencies, not CPU readbacks or mandatory full-device stalls. Their actual cost depends on hardware, scheduling and workload.
- [GfxRenderer](../engine/src/Renderer/GfxRenderer.cpp): AppendPasses is a no-op; old GfxFilter is retired despite legacy configuration comments. [ComputeFilterBackend](../engine/src/Renderer/ComputeFilterBackend.cpp) is unavailable pending a port. Do not assume direct draws currently use culling HZB.
- [visibility_hiz.h](../engine/include/Engine/Renderer/visibility_hiz.h) defines shared reversed-Z MIN pyramid storage, per-flight ownership, and SPD/per-mip contracts. At 1920x1080 its full-resolution chain has 12 levels. SPD can replace those 12 build dispatches with one, excluding counter setup and external edges.
- The reported Bistro coarse shadow-cull timing (0.000 ms, 2909 candidates) is a rounded observation, not proof of zero work or of a GPU barrier bottleneck. CPU culling, submission, GPU work and scheduling must be measured separately. HZB cost also depends on resolution; fixed dispatch count does not mean fixed GPU time.

## Implemented: candidate A (scoped compute dependencies)

Enable with TOURMALINE_GPU_SCENE_SCOPED_BARRIERS=1; unset or 0 selects the original global barrier. The log [GpuSceneSync] confirms the experiment is active. Changes are limited to the main cull chain and non-fused cascade builder. Fused visibility, fused cascade, temporal history and other engine callers retain existing barriers until their BDA publication contracts are audited.

CommandBuffer::EmitComputeMemoryBarrier uses COMPUTE_SHADER / SHADER_WRITE -> COMPUTE_SHADER / SHADER_READ | SHADER_WRITE. A generated indirect dispatch also includes DRAW_INDIRECT / INDIRECT_COMMAND_READ at the destination. The global memory barrier form is retained for BDA-only buffers; resource-specific barriers are not required solely to narrow stage scope. Do not remove true RAW/WAW dependencies. WAR still requires execution ordering.

Audit prior transfer/host/graphics producers, especially uploads, buffer clears and BDA-only resources. A compute-only intra-pass edge cannot substitute for missing entry publication, or for an exit edge to draw indirect, vertex/task/mesh shader, index input or transfer/readback. Existing graph hints are unchanged. If an external dependency previously relied on incidental global ordering, declare it explicitly before making this default. Keep the global fallback until validation succeeds.

| Edge | Required destination |
| --- | --- |
| Cull/count/prefix/scatter/work/draw build | COMPUTE_SHADER, storage read/write as needed |
| Indirect args -> DispatchIndirect | DRAW_INDIRECT / INDIRECT_COMMAND_READ plus compute shader for payload |
| Draw build -> indexed indirect | DRAW_INDIRECT / INDIRECT_COMMAND_READ |
| Work records -> graphics shader | Actual vertex/task/mesh stage / SHADER_READ |
| Depth attachment -> HZB | COMPUTE_SHADER / sampled read, proper image layout and depth-write source stages |
| HZB -> cull | Actual compute/task consumer / SHADER_READ |
| Diagnostic readback | TRANSFER / TRANSFER_READ (and host completion when applicable) |

## Further implementation candidates

### B. Fuse cull and compact count

Inspect gpu_scene_instance_cull.cs.hlsl and gpu_scene_compact_count.cs.hlsl. Emit per-64-candidate group counts from the cull shader, alongside the existing verdicts. Refactor early exits into inactive predicates so every lane reaches the workgroup reduction. Count exactly the same generation-valid visible verdicts as the existing kernel, including partial final groups and zero candidates. Preserve compact prefix/scatter ABI and deterministic ordering. Add an optional shader variant/push ABI, reflection checks and corresponding buffer registration. Apply the same approach to fused expansion only after parity. Expected structural saving: one dispatch and one inter-dispatch dependency, not a guaranteed timing win.

### C. Fuse prefix add and work scatter

gpu_scene_work_prefix_add.cs.hlsl adds each group base to local offsets and publishes offsets[candidateCount]. gpu_scene_work_scatter.cs.hlsl reads the resulting offset. Compute localOffsets[i] + groupBases[i / 256] in scatter, and publish the total from group scan or another well-defined producer. Audit every offsets consumer (including draw build, task/mesh, PSO count/scatter, verification and terminal sentinel) before eliminating the materialized global offsets. Do not race readers with writers. Preserve all capacity and overflow rules. Expected structural saving: one dispatch/barrier if no other consumer needs that pass.

### D. Verify and enable SPD

Existing culling controls: TOURMALINE_HIZ_SPD_VERIFY=1 for compare mode, then TOURMALINE_HIZ_SPD=1 for live SPD. The separate TOURMALINE_HZB_SPD family is for screen-space MAX HZB. Respect existing extent/program/counter readiness checks and per-mip fallback. Validate odd sizes, non-power-of-two extents, resize, counter reset, multiple in-flight frames, far depth and reversed-Z MIN reduction. No default change until byte parity and performance are established.

### E. Lightweight GPU-scene path

Prototype fused visibility/LOD/work generation with wave-aggregated atomic append into PSO-specific indirect buffers; consume through indirect-count where supported. Keep prefix compaction for large workloads. Compare contention, memory traffic, order sensitivity, capacity, PSO transitions, firstInstance/work identity and masked coverage. Establish conservative capacity admission before dispatch or an actual same-frame fallback on overflow; never silently clamp away draws. Reset counters with explicit dependencies. See gpu_scene_pso_prefix_compaction.md for existing design constraints. Do not assume one indirect draw covers all PSOs.

### F. Temporal HZB scheduling

Evolve existing seed/remainder/history, rather than add a parallel system. Consider previous completed HZB as a prediction for early rasterization, then build current MIN HZB using SPD and retest all deferred candidates against current occluder depth. Previous HZB alone is not a conservative rejection oracle after camera/object motion. Preserve generation checks, masked depth parity, history invalidation on cuts/resize and fail-open behavior. If history is invalid, rasterize conservatively. Profile whether overlap actually improves critical path. Current-frame HZB is still required for the retest; moving it later is not eliminating it.

### G. Direct path HZB

Keep direct path simple unless measurements show a benefit. Reuse existing pyramid infrastructure, not a second copy. CPU-issued direct draws cannot consume current GPU HZB without feedback or changing submission; avoid same-frame CPU readback. Explore conservative delayed feedback only with robust disocclusion handling, or a separate GPU-assisted indirect path. Existing screen-space MAX pyramid is not a replacement for culling MIN HZB. Camera HZB cannot blindly cull shadow casters; current optional swept-volume shadow HZB uses previous-frame depth and must remain conservative.

## Validation criteria / merge gates

### Correctness (mandatory before enabling scoped barriers by default)

- [ ] Clean C++ build and shader build on supported configurations; run existing GPU-scene candidate, visibility, PSO prefix, task/mesh and rendergraph contract tests.
- [ ] Vulkan synchronization validation enabled: zero new synchronization errors (RAW/WAR/WAW, layout, indirect reads); include explicit transfer/upload and counter-clear producers. BDA hazards may not be fully detected by validation, so also audit publication and use capture/oracle checks.
- [ ] Verify each changed call site has compute producer/consumer; confirm the indirect-verify edge includes DRAW_INDIRECT. Verify graph-managed output and input barriers in a GPU capture; no missing BDA publication.
- [ ] A/B with identical camera, settings, seeds and flight count; compare candidate verdicts, compact counts, work offsets including sentinel, indirect command fields, PSO counts, owner/generation and rendered depth/color against global-barrier baseline. Exact integer/command parity; pixel identity where deterministic (otherwise documented tolerance and no missing geometry).
- [ ] Cover indexed, task/mesh, meshlet compact, indirect verify, non-fused shadow cascades; fused and legacy fallback paths must remain unaffected. Confirm active backend and scoped log rather than accepting a silent fallback.
- [ ] Cover empty scenes, 1/63/64/65 and 255/256/257 candidates, capacity limits/overflow, high rejection, all visible, masked/double-sided, dynamic transforms, skinning, streaming/generation reuse, camera cuts, resize and multiple frames in flight.
- [ ] Use tools/gpu_scene_temporal_parity.py where applicable, including its engagement witness. SPD compare must be clean before live mode is considered; verify separate MIN vs MAX semantics.
- [ ] Stress repeated frames and randomized camera motion; no flicker, missing draws, stale counters, hangs or device loss.

### Performance (mandatory before default change)

Use release builds, fixed resolution, camera path, feature settings and hardware/driver; disable validation during timing. Warm up, then collect at least 300 frames per run, at least 3 independent runs per variant. Record median and p95 CPU frame/submission time, GPU frame time, scene cull/build, shadow build, seed, HZB, remainder and draw times, dispatch/barrier counts, candidate/work/visible draw counts, and GPU queue overlap. Report run-to-run variability and captures. Test Bistro and a representative large scene, preferably multiple GPUs.

| Variant | Settings |
| --- | --- |
| Direct | existing U-toggle direct path |
| GPU scene baseline | scoped=0, same temporal/SPD settings as control |
| Scoped only | TOURMALINE_GPU_SCENE_SCOPED_BARRIERS=1 |
| Temporal off | TOURMALINE_GPU_SCENE_TEMPORAL=0 |
| SPD only | TOURMALINE_HIZ_SPD=1 after verify |
| Combined | scoped=1, SPD=1, temporal enabled |
| Future fusions / append | separate switch per candidate; compare independently |

Proposed acceptance target (not a measured result): statistically repeatable improvement in GPU scene-build/total GPU time on Bistro, with no meaningful p95 total-frame or large-scene regression (initial tolerance 2% or measured noise floor, whichever is larger). Report both CPU and GPU critical paths; do not infer crossover from coarse shadow timing alone. If no significant benefit, retain experiment opt-in or revert. Do not combine algorithm changes with synchronization changes until each is independently validated.

## Rollout

1. Land experimental scoped dependency seam after code review/build; leave default global.
2. Run correctness and performance gates; fix missing graph edges, if any.
3. Independently validate SPD and promote only on supported configurations.
4. Implement B/C in separately bisectable changes; then evaluate E/F.
5. Re-evaluate direct HZB only after establishing the crossover with measured data.
