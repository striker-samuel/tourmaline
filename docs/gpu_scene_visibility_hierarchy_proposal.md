# RFC: GPUScene-owned sphere visibility tree and multi-view GPU culling

**Status:** design proposal; no shader, renderer, or persistent ABI changes in this PR.  
**Scope:** GPUScene visibility acceleration for Main, directional cascade RenderViews, and eventually spot/point/custom views.  
**Decisions:** preserve flattened static world transforms; maintain a **separate visibility tree**; **no AABB-based hierarchy or culling**; perform **visibility hierarchy traversal and candidate production entirely on the GPU**. CPU SIMD traversal is explicitly **not** a part of the proposed production path.

**Related:** #371 (GPUScene CSM); `gpu_scene_hlod.md` (geometry LOD, a separate tree).

## The actual problem

`bistro_grid32` has ~9.1M scene nodes and ~3M meshes. Existing #371 uses `SelectPassedInstanceRangesFrame` once for each cascade, after `LeafTicketsFrame`. That CPU selector sorts duplicate-ticket keys, scans coarse leaves and emits `PassedInstanceRange` streams separately for each view. Even with GPUScene performing per-instance GPU culling, its *entry point* is still CPU-driven. A CPU visibility BVH would make that CPU work faster but leave the wrong ownership boundary.

**Proposed replacement:** publish GPUScene spatial data when it changes; every render frame, GPU compute traverses a balanced sphere hierarchy, classifies all active RenderViews, and builds GPU-resident per-view candidate/visibility streams. The CPU must neither traverse visibility nodes, enumerate visible leaves, nor read back visibility lists to decide per-object draws.

Reported five-frame reference:

| Configuration | Avg frame | CPU | GPU | Shadow raster | Passes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Both direct | 2280 ms | 1948 ms | 183 ms | 0.14 ms | 8.9 ms |
| GPUScene shadows, motion direct | 3090 ms | — | — | 2.98 ms | — |
| GPUScene shadows + motion | 3020 ms | 2289 ms | 112 ms | 2.73 ms | 54 ms |

The +341 ms CPU difference and ~42 ms GPU preparation cost are **observations**, not proof that the entire delta is leaf selection. Instrument stages before attributing results. Moving selection to the GPU targets the CPU bottleneck; an efficient hierarchy and compact GPU work target the compute bottleneck.

## Two independent graphs, one GPUScene

```text
AUTHORING / TRANSFORM DEPENDENCIES (CPU)          VISIBILITY (GPUScene)
Scene::hierarchy                                 GPU-resident sphere tree
  static: flattened / baked                       static tree (immutable topology)
  dynamic: dirty parent propagation                dynamic tree (refit / relocate)
         |                                                |
         +---------- changed instance rows --------------+
                      {slot, generation}
                              |
                        GPUScene Database
                              |
        GPU multi-view sphere traversal + per-view outputs
                              |
      GPU instance/material/caster checks + LOD/work + indirect
                              |
                    Main and shadow RenderViews
```

Rebalancing a visibility node never reparents an authored transform. Transform updates only publish changed transforms/bounds and stable instance identities. The GPU visibility tree is *derived acceleration data*, not a second source of scene truth.

## Sphere representation, no axis-aligned boxes

Tourmaline already owns `Instance::localSphere` and a double-float world translation with basis vectors. For local center `c`, radius `r`, world translation `T`, and linear basis `B`:

```text
worldCenter = T + B*c
worldRadius >= r * sigma_max(B)
```

Use a proven conservative spectral-norm upper bound (e.g. sqrt of the maximum absolute row sum of `B^T B`), not the maximum column length under shear. Transform in adequate precision, produce origin-relative sphere centers for GPU arithmetic, and expand radius for float quantization. A nonfinite or incomplete bound is *uncullable*, never invisible. Skinned/morphing geometry must use a conservative animated/swept bound or be flagged uncullable.

Two enclosing child spheres `(c1,r1)` and `(c2,r2)` are combined as follows: if one completely contains the other, keep it. Otherwise `d=|c2-c1|`, `R=(d+r1+r2)/2`, and `C=c1+(R-r1)/d*(c2-c1)`. Expand outward for representation error. The hierarchy builder must verify that each parent encloses every child.

### Conceptual GPU ABI, not a committed struct

```cpp
struct GpuSphere {
    float3 centerRelativeToRegionOrigin;
    float radius;
};
struct GpuSphereNode4 {
    float4 cx, cy, cz, radius; // four child spheres (SoA)
    uint4 childOrLeaf;        // node index / leaf handle
    uint4 flags;              // internal, leaf, incomplete, etc.
};
struct GpuVisibilityLeaf {
    uint firstRef;
    uint refCount;
    uint generation;
    uint flags;
};
struct GpuVisibilityRef {
    uint instanceSlot;
    uint instanceGeneration;
};
struct GpuVisibilityView {
    float4 planes[6];         // same region-relative frame as sphere centers
    uint flags;               // Main / cascade / etc.
    uint viewId;
};
```

Exact packing must be verified against DXC SPIR-V and the existing `gpu_platform.hlsli` BDA conventions. The *region origin* is stable per published chunk; use high/low split at the region boundary for large worlds. Do not combine floats from different origins without explicit rebasing. The view plane equation is inward `dot(n,p)+w >= 0`.

The existing `GpuSceneLeaf` currently stores `worldCenter + halfExtents` and the `PassedInstanceRange` GPU shader expects a generational leaf ID plus contiguous member range. **Those are legacy ABIs, not part of the proposed sphere tree.** An initial compatibility bridge may materialize the *same* per-view `PassedInstanceRange` structure **on GPU** from published leaf/member rows; new direct GPU ref streams require a versioned consumer shader, not a reinterpret cast. No CPU reconstruction of per-view ranges.

## Balanced tree design

A **wide sphere BVH4** is a working candidate, not an AABB BVH. Prioritize spatial locality, small enclosing radii, low sibling overlap and bounded leaf occupancy over exact equality of depth.

- **Static:** build topology at cook time or once at world load; Morton ordering of sphere centers is a possible fast initial grouping. Compare sphere-inflation-aware bottom-up clustering or localized refinement if Morton groups become too loose. Upload immutable nodes, leaf references, root index and generation/epoch.
- **Dynamic:** separate GPU-resident loose-sphere tree or bounded update buckets; updates arrive as dirty instance IDs + transformed spheres. GPU compute refits affected leaves and ancestors **bottom-up in reverse level order**. Bound re-insertion/rebalancing work and use a safe rebuild/swap when quality drops; never expose partially refitted nodes to concurrent render traversal.
- **Uncullable:** a separate GPU-visible list for unknown bounds, invalid identity, large moving/skinned objects lacking conservative limits; downstream generation checks still apply.
- **Update quality:** collect depth, nodes visited, sphere inflation, overlapping child coverage, occupancy and dynamically relocated objects. A balanced depth alone is not a performance criterion.

A CPU-side offline/cook builder is acceptable for *static construction*; **per-frame culling and visible-list generation are not**. If runtime dynamic topology must change, a GPU staged rebuild is preferred, with a complete immutable old snapshot available until the new snapshot is valid. Don't require per-frame CPU breadth-first traversal.

## GPU traversal: bounded queues + multi-view masks

Use **level-batched, GPU breadth-first traversal** (one frontier per level). A persistent worker queue is a possible later optimization, but start with predictable multi-dispatch levels and efficient GPU prefix/compaction. SIMD lanes are **GPU wave/subgroup lanes**, not CPU AVX.

Each frontier entry stores `nodeIndex`, `activeViewMask`, and `fullyInsideMask`. Initial mask represents the views active this frame. Every node tests up to four child spheres and the still-partial views:

For sphere center `C`, radius `r`, inward plane `(n,w)`, let `d=dot(n,C)+w` and `e=r*length(n)`:

- Outside view if any plane has `d+e < -epsilon`: clear that view's bit.
- Fully contained in view if all planes have `d-e >= epsilon`: set its inside bit; descendants skip further plane tests *for that view*.
- Otherwise partial: propagate view's active bit to child.
- Invalid/unknown bounds: fail open, never reject.

Conservative `epsilon` covers numeric error and region-relative origin conversion. A node can be outside Main and visible in C2. **Never filter shadows through Main visibility.**

### Proposed GPU pass graph

```text
GpuScene upload / changed-sphere publication (only dirty records)
    |
    v
GpuVisibilityRefit [when dirty; static topology normally unchanged]
    |  compute write -> compute read
    v
GpuVisibilityInit [root + active RenderViews, reset counters/flags]
    |
    v
GpuVisibilityTraverseLevel0
    |  wave ballots + block counts + prefix/compaction
    v
GpuVisibilityTraverseLevel1...N
    |  GPU dispatch-indirect of produced frontier counts
    |  each wave classifies child spheres for active view mask
    v
GpuVisibilityLeafCount [per-view counts and overflow flags]
    |
    v
GpuVisibilityLeafScatter [compact leaf/ref output per view]
    |
    v
GpuSceneInstanceCull / owner + generation + shadow caster
    |
    v
GPUScene work select -> GPU prefix -> indirect command build
    |
    v
Main / CSM indexed indirect raster
```

For DXC/Vulkan feasibility: **dispatch-indirect argument generation is GPU-side** and must have explicit shader-write -> indirect-read dependencies. A zero-entry frontier is represented by a valid zero-group dispatch or a no-op branch according to the target Vulkan requirements. Avoid an unrestricted global atomic append per candidate; use wave/block aggregation then prefix-scan and scatter. Bound wave output by explicit frontier/output capacity.

**Important pipeline design issue:** the existing `CandidateValidator::Prepare` consumes CPU `Selection.ranges` and CPU candidate count and sizes scratch from that count. A real GPU-driven path cannot reuse that interface unchanged. Add a GPU-input publication/dispatch API with **capacity-sized per-flight scratch**, GPU-produced counters and indirect dispatches, while retaining `Prepare` as the legacy debug/validation path. Existing kernels assuming a CPU-known candidate count must be revised to consume device-produced counts, with all bounds checks remaining active.

### Compact per-view output without five duplicated tree walks

Share *tree loads and frontier traversal*. Output a per-leaf view mask first, then compute per-view counts and prefixes on GPU and scatter the relevant leaf/member references. Main and four cascades have independently compacted outputs even though the parent tree is traversed once.

**Do not immediately expand accepted subtrees into individual instance IDs.** Mark a subtree as fully contained for selected views, retain its leaf-range metadata, and lower it using GPU leaf range enumeration or a flattened subtree-to-leaf index. Where old `LeafMember` spans are not contiguous, preserve a short list of actual member ranges and compute device prefixes. A future packed adjacency publication can make accepted subtree refs contiguous, but that is an ABI change that must be benchmarked and versioned.

A fully-contained subtree skips *further frustum tests*; it never skips per-instance generation, geometry residency or shadow eligibility checks. Per-cascade caster flags remain independent.

## No CPU visibility readback; correct overflow/failure semantics

The most important migration contract is **no invisible partial result** on buffer overflow.

GPU traversal detects frontier capacity, per-view leaf/range count, candidate count, and indirect work overflow **before** publishing a view as safely culled. Counters and error bits live on GPU; the render graph does not wait for a CPU readback.

A CPU-only legacy draw fallback triggered after a GPU overflow would require a readback/stall and is **not a valid zero-stall plan**. Instead choose an explicit, correct GPU-safe policy:

1. **GPU conservative fallback:** prepare a prevalidated all-owned-instance/index work path with bounded indirect dispatch/command paging; if the hierarchy or capacity fails, use this conservative path (possibly slower) for that view, *without silently dropping any owned instance*. Unowned geometry remains direct.
2. **Or** perform a conservative whole-view direct pass **unconditionally** during a probationary/shadow mode until a GPU-driven fallback can be proven; no GPU-owned source may be drawn twice when both paths participate.
3. **Or** reject GPUScene ownership **before the frame**, using CPU-known allocation/structural admission proofs where sufficient, and use direct for that view. A GPU-only runtime overflow cannot safely be handled by a decision made too late on CPU.

These are alternatives requiring explicit implementation and testing, **not automatic behavior of the proposed pipeline**. Per-view GPU selection must be all-or-safe-fallback. Never just cap `1,048,560` candidates and return partial visibility. Never depend on a blocking GPU->CPU readback in normal frames.

Use Vulkan per-flight ownership and RenderGraph hazards for tree snapshot publication, frontier counters, node refits, GPU range writes, dispatch-indirect reads, GPU work construction and draw-indirect reads. Avoid mutating a tree snapshot still being read by earlier flights; double-buffer or version/swap it.

## Multi-view correctness

Per-frame GPU view descriptors are produced from the established RenderView system. Directional cascade culling uses the full conservative **light-space caster** volume, including off-camera casters, not the main camera frustum or only visible receivers. A camera-rejected node may remain active for several shadow cascades. Missing/disabled planes fail open.

Test union/mask correctness against the existing CPU `SelectPassedInstanceRangesFrame` oracle on small test scenes, but compare **visible-reference set inclusion**, not exact candidate counts: conservative spheres may legitimately include extra objects. In all cases, the final GPU visible output must contain every genuinely visible instance eligible for that view.

## Implementation plan in independent checkpoints

### P0 — baseline and GPU ABI

- Time the existing CPU ticket snapshot + per-cascade selector, current GPU `preC`, and GPU ownership/fallback separately.
- Add an experimental, fully GPU-output `GpuVisibilityFrame` interface with device counters, capacity sizes, view masks and epoch. Keep existing `CandidateValidator::Prepare` untouched for validation and direct fallback.
- Add GPU readback **only in tests/debug** to compare oracle leaf/instance sets. Probe DXC push/layout constraints, minimum dispatch limits and exact RenderGraph barrier stages.

### P1 — flat GPU multi-view selector (no tree yet)

- Upload/retain the current immutable sphere-leaf table and generational member ranges once per relevant topology revision; only dirty bounds on change.
- One GPU compute pass classifies each leaf against Main+C0..C3, writes a per-leaf view mask.
- GPU wave/block prefix + scatter produce independent per-view candidate lists and counters. No CPU leaf scan, sorting, or per-view range serialization.
- Run GPU instance cull from GPU-produced counts via GPU dispatch-indirect. Implement the proven safe overflow policy **before enabling owned draws**.
- A/B all-candidates, CPU flat selector, and GPU flat selector; report both GPU time and render-thread CPU time.

### P2 — balanced GPU sphere tree

- Publish immutable static sphere tree and leaf adjacency. Add bounded level-by-level GPU wave traversal, per-view inside/partial masks, and GPU frontier prefix compaction.
- A/B GPU flat against GPU tree for `bistro_grid32`, small scenes, interiors and large-origin scenes. Measure sphere loose-volume costs and skip tree when flat is demonstrably cheaper.
- Only then adopt packed whole-subtree acceptance paths.

### P3 — dynamic tree/refit

- GPU dirty sphere update, bottom-up refit, staged reinsert/rebuild and per-flight safe swap.
- Keep skinned/unknown bound handling conservative. Validate retire/reuse, region relocation and temporal stability.

### P4 — GPU work reuse

- Optimize GPU `preC` separately: avoid redundant LOD/residency/geometry work only when equivalent across views; keep distinct per-view caster and frustum filtering.
- Benchmark shared work against four independent GPU command streams and measure net wall-clock benefit, not just raster time.

## Acceptance and instrumentation

Counters: nodes per level, frontier peak, views surviving per node, sphere inflate ratio, leaf mask distribution, per-view refs/candidates, total GPU memory and per-flight scratch peak, overflows/fallback type, static build and dynamic refit cost.

Timings: render-thread CPU (including remaining direct fallback), visibility GPU init/traverse/scan/scatter, cull, work selection, indirect build, Main and cascades, full wall-clock. Compare same five-view scene and camera positions with the existing CPU oracle.

Safety: no false-negative camera or shadow culling; no partial ownership; no stale generations; conservative nonuniform/sheared transforms and >1e9 origin handling; valid masked/skinned/dither semantics; no unsafe queue or indirect buffer overwrite. Test all paths on DXC, Vulkan validation layers, multi-flight frame reuse, streaming changes and forced capacity overflow.

**Success:** meaningful reduction in `bistro_grid32` host CPU selection (ideally close to zero steady-state visibility traversal on CPU), without increasing GPU visibility/preC enough to lose wall-clock performance. Any measurable win must include the cost of GPU prefix/scatter and extra candidate over-admission. Retain a correct fallback if GPU visibility is absent/disabled.

## Non-goals

- CPU SIMD or CPU BVH traversal as the production visibility path.
- Axis-aligned bounding boxes as nodes or tests of the new visibility tree.
- Reconstructing visible lists on CPU each frame.
- Altering the authored transform hierarchy or undoing static flattening.
- Reusing main-camera visibility for cascades.
- Treating this RFC as an implemented performance optimization.
