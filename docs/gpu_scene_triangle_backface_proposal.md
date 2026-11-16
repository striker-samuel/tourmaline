# Proposal: brute-force triangle backface culling in GPU Scene mesh shaders

**Status:** proposal only; no runtime/shader behavior changed by this PR.
**Reference:** Arseny Kapoulkine, [Fine-grained backface culling](https://zeux.io/2023/04/28/triangle-backface-culling/) (2023).

## Problem and hypothesis

GPU Scene already performs conservative meshlet-level sphere/frustum, optional cone and Hi-Z rejection in `engine/shaders/gpu_scene_meshlet.task.hlsl`. Surviving meshlets are emitted as indexed triangles by `engine/shaders/gpu_scene_meshlet.mesh.hlsl`. Cone rejection skips entire mesh workgroups but cannot reject back-facing triangles within surviving mixed-orientation meshlets. Hypothesis: per-primitive backface rejection in the mesh stage reduces downstream primitive setup/raster work enough to pay for shader ALU and scratch overhead on geometry-heavy views.

The technique is **backface culling, not triangle occlusion**. Keep cluster culling before mesh dispatch; do not replace it. The cited article measured a benefit for cone + brute-force culling in one workload, but that does not establish a Tourmaline speedup.

## Proposed scope

1. Add a separately selectable **off / brute-force backface** mode to the GPU Scene task/mesh raster implementation. Default **off** until validated. Preserve the indexed fallback and all existing ownership/residency gates. Do not silently enable for legacy mesh shader paths.
2. In `gpu_scene_meshlet.mesh.hlsl`, reuse already transformed `GsMeshVertex.pos` (clip-space `SV_Position`) and meshlet-local packed triangle indices. Publish the clip-space `xyw` values into a per-workgroup scratch array (or another measured safe data-sharing strategy) and synchronize before consuming them. Keep indexed vertex reuse; do **not** expand to three vertices per triangle.
3. For each triangle, compute the sign of the homogeneous screen-space determinant, e.g. `dot(a, cross(b,c))` for `a,b,c = clip.xyw`. Write a per-primitive HLSL `SV_CullPrimitive` semantic (SPIR-V `CullPrimitiveEXT`) for triangles proven backfacing. Preserve `SetMeshOutputCounts(vertexCount, triCount)` initially; this feature flags primitives rather than compacting or changing output counts.
4. Confirm the sign empirically against the Vulkan viewport, `frontFace` raster state and index winding. **Do not copy the blog's determinant sign unchecked.** For triangles crossing or behind the eye/near clip plane, nonfinite clip positions, near-zero determinant, or unsupported orientation, **fail open** (do not manually cull); fixed-function clipping/culling remains authoritative.
5. Never manually reject triangles on two-sided raster classes. Honor mirrored instance transforms, including negative determinant, by testing the *transformed clip-space* vertices and the actual front-face state. If front-facing definition varies by PSO, provide the correct sign per PSO or disable this optimization for that class pending validation.
6. Invalid meshlet-local indices and malformed metadata remain a CPU admission/whole-submesh indexed fallback concern, **not** a new opportunity to silently drop geometry in the shader. Shader-level defensive guards are supplementary only.
7. Make the mode visible in captures/benchmark scenario configuration (e.g. a documented `TOURMALINE_GPU_SCENE_TRIANGLE_BACKFACE` pin, subject to the project's runtime-toggle convention). Avoid new scene push-constant fields or ABI changes where possible. Ensure depth prepass, main/G-buffer and relevant shadow/motion paths are consistent if they share the mesh raster implementation; otherwise scope the flag to explicitly validated passes.
8. No precomputed masks, per-triangle occlusion tests, GPU atomic append, primitive prefix compaction, or material-specific geometry splitting in this first experiment.

### Illustrative pseudocode (not drop-in HLSL)

```hlsl
// In mesh vertex loop, after FetchVertex and clip transform:
clipXYW[v] = verts[v].pos.xyw;
// Workgroup synchronization: all producers have completed before readers.

// For each valid local triangle in a single-sided raster class:
float3 a = clipXYW[ids.x], b = clipXYW[ids.y], c = clipXYW[ids.z];
bool safe = all(isfinite(a)) && all(isfinite(b)) && all(isfinite(c))
         && a.z > epsilon && b.z > epsilon && c.z > epsilon;
float area = dot(a, cross(b, c));
bool cull = safe && isfinite(area) && abs(area) > areaEpsilon
         && IsBackFacing(area, rasterFrontFaceConvention);
primitive[t].culled = cull; // SV_CullPrimitive, per-primitive output
tris[t] = ids;               // original indexed output
```

The `epsilon`/area safeguards must be selected and tested against clip-space scale, precision, and near-plane behavior; overly large epsilon only reduces optimization effectiveness, but an incorrect sign or unsafe test causes missing geometry. Avoid divergent group barriers: all active lanes must reach the barrier, including threads with no assigned vertices.

## Validation criteria / merge gate for the later implementation PR

- [ ] **Shader/toolchain:** DXC HLSL -> SPIR-V compilation passes for task/mesh pipelines; inspect reflection/disassembly for per-primitive `CullPrimitiveEXT` and proper array size/decoration. Vulkan validation layers report no mesh-output, synchronization or descriptor errors. Test supported mesh/task feature and mesh-output limits; unsupported devices use existing fallback.
- [ ] **CPU oracle:** Determinant orientation tests match a CPU reference for both triangle windings, translated/rotated/projected triangles, degenerate triangles, transformed/mirrored instances, `w <= 0` and near-plane intersections, nonfinite inputs, and extreme camera-relative coordinates. Cases that cannot safely reject must fail open.
- [ ] **Material/PSO parity:** Opaque/masked and backface/two-sided classes; alpha cutout and A2C; correct winding in all viewport orientations, negative-scale instances, and skinned instances. Two-sided primitives must remain unculled by the new test. Existing meshlet task cone and Hi-Z toggles must compose correctly.
- [ ] **Raster parity:** Compare culling **off vs on** in depth, G-buffer/Main (and relevant shadow, motion, temporal seed/remainder paths when owned by task/mesh) with reproducible camera paths. Use Sponza, Bistro (when assets available), mixed-winding/thin-geometry scenes, near-plane sweeps, inside-geometry views, and heavy overlap. No unexplained missing pixels, changed depth silhouettes, holes or flicker; quantify image deltas with the project's pixel compare/snapshot tooling. Known indexed-compact baseline parity issues must be reported separately, not attributed to this optimization.
- [ ] **Fallback/ownership:** Invalid/stale meshlet streams, nonresident geometry, capacity overflow, multi-material/PSO work boundaries, and unsupported hardware still render through indexed fallback without duplicates or disappearance. Test >32 meshlets per task series, 64/96-triangle bounds, zero/degenerate triangles, and the 1,025/4,096-meshlet scenarios documented in the task/mesh path.
- [ ] **Performance isolation:** Benchmark at minimum **(A)** meshlet cone off / triangle off, **(B)** cone on / triangle off, **(C)** cone off / triangle on, **(D)** cone on / triangle on, with unchanged Hi-Z, LOD, visibility freeze, resolution, clock policy and scene/camera. Record GPU frame and pass times plus primitives emitted/rejected and mesh/task invocations. Compare CPU-driven indexed baseline separately.
- [ ] **Measurement quality:** Use `tools/perf_harness.py` with repeatable `benchmarks/scenarios.json` cases, warmup and repeated runs, median and variance; GPU traces/pipeline statistics when available. Capture at least one geometry-bound and one non-geometry-bound scene on available supported NVIDIA/AMD hardware; disclose missing vendors rather than infer speedups. Report extra shader instructions/registers/shared memory and occupancy. Retain a disabled-by-default mode if performance is neutral or negative.
- [ ] **Regression and docs:** Run applicable CPU tests, shader build, `python -m unittest discover -s tests -p "test_text_integrity.py" -v`, Vulkan validation, scenario catalog/harness checks, and update `docs/gpu_scene_task_mesh_raster.md` once integrated. Record hardware, drivers, shader flags, frame captures and precise pass coverage in the implementation PR.
- [ ] **Adoption gate:** Enable by default only with demonstrated raster correctness and reproducible improvement on representative target hardware and no substantial regressions. Otherwise keep opt-in or revert. No unmeasured percentage or FPS claim.

## Suggested implementation stages

1. Establish a **known-good task/mesh raster baseline**: build, validation, and depth/Main parity with the indexed reference. This proposal does not resolve existing baseline issues.
2. Implement per-primitive output semantics and a conservative orientation oracle, behind an opt-in switch.
3. Validate visibility/correctness across the matrix above before performance tuning.
4. Instrument rejected primitive counts and profile the four cone/triangle combinations; choose default based on measured value.
5. Only then consider secondary techniques (small-primitive rejection, primitive compaction, alternate scratch layout), each as a separate experiment.

## References

- [Fine-grained backface culling — zeux.io](https://zeux.io/2023/04/28/triangle-backface-culling/)
- [GPU Scene task/mesh raster path](gpu_scene_task_mesh_raster.md)
- [Mesh stage](../engine/shaders/gpu_scene_meshlet.mesh.hlsl)
- [Task stage](../engine/shaders/gpu_scene_meshlet.task.hlsl)
