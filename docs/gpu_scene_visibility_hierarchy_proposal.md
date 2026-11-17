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
GpuVisibilityTraverseLevel0...N
    |  sphere vs frustum, multi-view active/inside masks
    |  wave ballots + block counts + frontier prefix/compaction
    v
GpuVisibilityLeafMask [one uint32 mask per leaf, all views]
    |
    v
LeafWorkQueue [compact ACTIVE leaf refs + instance range + view mask]
    |  block/leaf-level prefix — NEVER per-instance scatter
    v
FusedInstanceWork [load instance + mesh + LOD + geometry ONCE]
    |  generation + ownership + CastShadow, once
    |  evaluate the leaf's active views; shared resident-LOD policy
    v
SubgroupViewAggregate [per-view ballot, block counts, reserved output]
    |
    v
Per-view compact indirect bins [only surviving draw work]
    |
    v
Main / CSM indexed indirect raster
```

The decisive cost is the **5× instance amplification** in the four independent cascade expansion chains, not the leaf scan. Compacting *leaves* (one 32-bit mask each) and fusing instance loading/LOD/residency across views replaces four per-cascade `cull -> compact -> work -> scan -> drawbuild` pipelines with one leaf-driven pass that reads each instance once and emits only surviving per-view draw work.

For DXC/Vulkan feasibility: **dispatch-indirect argument generation is GPU-side** and must have explicit shader-write -> indirect-read dependencies. A zero-entry frontier is represented by a valid zero-group dispatch or a no-op branch according to the target Vulkan requirements. Avoid an unrestricted global atomic append per candidate; use wave/block aggregation then prefix-scan and scatter. Bound wave output by explicit frontier/output capacity.

**Important pipeline design issue:** the existing `CandidateValidator::Prepare` consumes CPU `Selection.ranges` and CPU candidate count and sizes scratch from that count. A real GPU-driven path cannot reuse that interface unchanged. Add a GPU-input publication/dispatch API with **capacity-sized per-flight scratch**, GPU-produced counters and indirect dispatches, while retaining `Prepare` as the legacy debug/validation path. Existing kernels assuming a CPU-known candidate count must be revised to consume device-produced counts, with all bounds checks remaining active.

### Compact leaves, not instances — the leaf work queue

Share *tree loads and frontier traversal*, and output a **per-leaf view mask** (one `uint32` per leaf, never a per-instance scatter). The classifier's result (`uint leafViewMask[leafCount]`) is the compact output: 65,536 leaves produce 256 KiB of masks, whereas five per-view 16-byte verdict streams over 3 M instances would emit ~229 MiB of writes alone.

The next stage is a **leaf work queue**: compact the *active leaf references* (leaf index + its stable instance range + view mask) into a small work list using a leaf/block-level prefix — not a per-instance, per-view prefix/scatter. Each active leaf's instance range is stored once, not expanded into five candidate arrays.

A **fused instance pass** then walks each queued leaf's instance range, loading each instance (and its mesh/LOD/geometry) **once**, validating generation/ownership/CastShadow once, evaluating the leaf's active views, and emitting only surviving per-view draw work. The LOD/residency choice follows a proven common policy (or a cheap per-view refinement) — the camera LOD decision must never be silently reused for shadow cascades.

**Do not immediately expand accepted subtrees into individual instance IDs.** Mark a subtree as fully contained for selected views, retain its leaf-range metadata, and lower it using GPU leaf range enumeration or a flattened subtree-to-leaf index. A future packed adjacency publication can make accepted subtree refs contiguous, but that is an ABI change that must be benchmarked and versioned.

A fully-contained subtree skips *further frustum tests*; it never skips per-instance generation, geometry residency or shadow eligibility checks. Per-cascade caster flags remain independent.

**Avoid global atomics per draw.** Assign one or more workgroups per active leaf/chunk, use subgroup ballots to determine per-view survival, aggregate counts per block, reserve output via aggregated atomics (or a block-level prefix where stable ordering is required), and scatter only surviving work. A global prefix over millions of instances becomes a prefix over far fewer workgroups. Where GPUScene temporal visibility relies on persistent ordering, preserve stable instance identity and map compacted commands back to their work rows.

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

### P1 — flat leaf classifier + fused multi-view instance work (no tree yet)

- Upload/retain the current leaf table (AABB bounds) and generational member ranges once per relevant topology revision; only dirty bounds on change.
- One GPU compute pass classifies each leaf against Main+C0..C3, writes a per-leaf view mask (`uint leafViewMask[leafCount]`).
- Compact **active leaves** into a leaf work queue (leaf index + stable instance range + view mask) via a leaf/block-level prefix — never a per-instance, per-view scatter.
- A **fused instance pass** loads each instance/mesh/LOD/geometry once, validates generation/ownership/CastShadow once, evaluates the leaf's active views, and emits per-view draw work via subgroup ballots + block aggregation (no per-draw global atomics).
- Implement the proven safe overflow policy **before enabling owned draws**. A/B the fused pipeline against the four independent cascade chains on `bistro_grid32` (report GPU `preC` and render-thread CPU).

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

## Concrete GPU data layout and costs (implementation appendix)

These layouts are **proposals**, not compiled, reflected or benchmarked ABIs. Keep the old \`Instance\`, \`Leaf\`, \`LeafMember\` and raster-work layouts byte-identical during the first migration. Verify all new \`sizeof\`, \`alignof\`, \`offsetof\` and HLSL compiler offsets in one shared ABI test.

### Persistent versus per-flight storage

| Buffer | Proposed element | Ownership | Update trigger |
| --- | --- | --- | --- |
| GPUScene instances | Existing \`Row<Instance>\` (144 B including identity) | Existing per-flight uploader | Dirty instance/mesh |
| Static sphere topology | 96 B \`SphereNode4\`, 16 B \`VisibilityLeaf\`, leaf index/adjacency | New visibility publisher | Topology/geometry revision |
| Dynamic bounds/tree | 16 B local-to-region spheres and compact parent/leaf indices | GPU compute, immutable snapshot for readers | Dirty transforms / movement |
| View descriptors | Existing 128 B cull view + view flag/epoch metadata | RenderViewSet publication | Per frame |
| Frontiers | Double-buffered 16 B node+view-mask records | Per-flight scratch | Every traversal |
| Leaf masks | 4 B per leaf (up to 32 views) | Per-flight scratch | Each frame |
| Per-view ranges | Existing 32 B \`PassedInstanceRange\` | Per-flight GPU output | Each frame |
| Per-view visibility/work | Existing verdict/work/indirect formats initially | Per-flight GPUScene downstream | Each frame |

\`\`\`cpp
// Conceptual v1 wire structures; no CPU pointers or platform bool.
struct alignas(16) SphereNode4 {
    float cx[4], cy[4], cz[4], radius[4];  // 0..63
    uint32_t childRef[4];                  // 64..79
    uint32_t childKind[4];                 // 80..95
}; // 96 B

struct alignas(16) VisibilityLeaf {
    uint32_t leafSlot;
    uint32_t leafGeneration;
    uint32_t firstMember;
    uint32_t memberCount;
}; // 16 B

struct alignas(16) Frontier {
    uint32_t nodeIndex;
    uint32_t activeViewMask;
    uint32_t insideViewMask;
    uint32_t pad;
}; // 16 B

struct alignas(16) Sphere {
    float regionLocalX, regionLocalY, regionLocalZ, radius;
}; // 16 B
\`\`\`

The sphere tree has a per-region double/high-low origin. Distances/planes must be rebased into the **same** frame; do not subtract unrelated float32 absolute-world coordinates. Use conservatively widened float radii. \`insideViewMask\` is transient scratch, never immutable node data. Explicit child types avoid deriving leaf/node identity from unchecked sentinel values.

**Contiguous descendants are not implied.** An internally accepted node can skip *plane tests*, but \`PassedInstanceRange\` demands real, valid \`LeafMember\` addresses and the current generational leaf identity. Until a versioned packed ref-stream consumer exists, the GPU must enumerate valid descendant leaves from a packed adjacency/index table. Design the adjacency so each subtree spans a contiguous *leaf-index* interval; this does **not** imply that its \`LeafMember\` rows are contiguous.

### Worked memory budget (capacity, not allocations observed)

Assume **65,536 leaves** (scenario, not an observed \`bistro_grid32\` leaf count), a full four-way tree with approximately 21,845 internal nodes, five simultaneous views, at most 65,536 emitted ranges and 1,048,576 candidates *per view*. MiB uses 1,048,576 B.

| Storage | Computation | Approx. |
| --- | --- | ---: |
| Sphere BVH4 internal nodes | 21,845 × 96 B | 2.00 MiB |
| Leaf descriptors | 65,536 × 16 B | 1.00 MiB |
| Double-buffer frontiers (if each holds <=65,536 entries) | 2 × 65,536 × 16 B | 2.00 MiB |
| Per-leaf view mask | 65,536 × 4 B | 0.25 MiB |
| Compatibility range stream, **one view** | 65,536 × 32 B | 2.00 MiB |
| Compatibility range streams, **five views** | 5 × previous | 10.00 MiB |
| Existing candidate verdicts, **one view** | 1,048,576 × 16 B | 16.00 MiB |
| Existing candidate verdicts, **five views** | 5 × previous | 80.00 MiB |
| Existing fixed 8-class indexed indirect command buffer, **one view** | 1,048,576 × 8 × 20 B | 160.00 MiB |
| Existing fixed buffers, **five views** | 5 × previous | 800.00 MiB |

**Critical caveats:** the 65,536-entry frontier is an *illustration*, not a proven worst-case queue bound; a four-way expansion can require more frontier entries transiently, and a shared traversal with several view-mask branches can inflate pressure. Prove maximum queue elements or allocate by a separately validated capacity; overflow must not silently drop children. The above omits scene storage, compaction counters/prefixes, LOD choices, work/demand buffers, transient geometry, GPU fallback, allocator overhead, flight replication and actual residency. The 800 MiB fixed-stride number is a major reason to prefer **compacted indirect draw counts** rather than retaining eight full padded command bins for each view. Multiply per-flight transient capacities by frames-in-flight *only if those buffers are actually independently allocated for each flight*.

The existing instance row costs 144 B × 3M instances ≈ 412 MiB, excluding other scene data; the scene has roughly 3M meshes but the **actual GPUScene instance count must be measured**, not equated unconditionally to mesh count.

### Memory and bandwidth model

A packed node fetch is 96 B. A full four-way tree with L leaves has at most approximately (L−1)/3 internal nodes in the full-tree case. One complete traversal of 21,845 internal nodes reads ~2.0 MiB of raw node payload **if each node is fetched once**; memory transactions, cache misses, masks and output writes add cost. An efficiently fused traversal can amortize **node reads**, but not necessarily each view's six-plane distance tests.

Let \`N\` = tested node-child spheres, \`V\` = active views, \`C_v\` = instances emitted for view v, \`R_v\` = leaf ranges emitted for view v, \`D_v\` = compact GPU draw commands for view v. Then a useful first-order model is:

- Sphere math: **O(N × V × up to 6 planes)**, reduced by early-out and \`insideViewMask\`.
- Leaf-range emission: **Σ R_v × 32 B** in compatibility mode.
- Verdict traffic: at least **Σ C_v × 16 B** for one write of every candidate verdict, plus read/compaction traffic.
- Compact indirect output: **Σ D_v × 20 B**, rather than \`8 × capacity × 20 B\` per view when fixed bins are written in full.
- Dispatch count: approximately one stage per tree level, plus queue scan/compaction per level, leaf mask/range expansion, instance cull, compaction, LOD/work, and command build. It is a **GPU synchronization and launch cost**, not just arithmetic.

For illustration only: a full tree with ~65k leaves is around 9 levels deep (\`ceil(log_4(65536)) = 8\` child-expansion levels plus the root). The actual number of dispatches depends on whether each level uses separate count/scan/scatter and on how sparse the active frontier is. Neither "eight levels" nor "one traversal" means "eight cheap dispatches".

### GPU pipeline integration with #371

\`\`\`text
CPU Scene hierarchy / transform journal
           |
           v
 GPUScene uploader: dirty instance rows, owner-generation mask
           |
           v
 GPU visibility snapshot / GPU dynamic sphere refit (if dirty)
           |
           v
 [one shared multi-view GPU visibility traversal]
   tree frontiers -> leaf view masks -> per-view range counts
           |
           v
 GPU per-view exclusive prefix + range scatter
           |
           v
 GPU dispatch argument generation (compute -> indirect-read barrier)
           |
           +--------------+--------------+--------------+
           |              |              |              |
           v              v              v              v
       Main view       Cascade 0      Cascade 1 ... Cascade 3
           |              |              |
       GPU instance cull, generation/owner checks, CastShadow gate
           |              |              |
       GPU compaction / resident LOD / work generation
           |              |              |
       GPU PSO bins + DrawIndexedIndirectCount, depth / masked
\`\`\`

The existing \`CandidateValidator::Prepare\` expects a CPU-built \`Selection\` with \`candidateCount\`; \`PrepareCompaction\`, \`PrepareWorkExpansion\` and \`PrepareParallelWorkPrefix\` also size and dispatch from a **CPU-known count**. Thus the change is **not** replacing one selector function while leaving the rest unchanged. Add an independent \`GpuVisibilityFrame\` runtime with capacity-proved GPU buffers and GPU-produced count/dispatch arguments; retain \`CandidateValidator\` as a CPU reference/legacy validation path. Modify or permute cull/compaction/work shaders to read live GPU counters, check counts against allocated capacity, and dispatch through generated arguments. The current \`gpu_scene_instance_cull.cs.hlsl\` binary-searches CPU-prefixed \`PassedInstanceRange\` entries; compatibility mode must generate monotonically increasing \`candidateBase\` on the GPU before it can be reused.

Suggested RenderGraph boundaries:

| Pass | Reads | Writes | Required dependency |
| --- | --- | --- | --- |
| VisibilityRefit (dirty only) | Dirty instances/spheres; previous safe tree | Next sphere snapshot | Compute write → traversal compute read |
| VisibilityInit | RenderViews, stable root, epoch | Initial frontier + zeroed counters/status | Compute write → traversal compute read |
| TraverseLevel | Sphere nodes, frontier, view planes | Next frontier, leaf masks, counters | Compute write → next compute/dispatch-indirect read |
| LeafCount/Scan/Scatter | Masks, leaf identities, member ranges | Per-view prefixed GPU range buffer + candidate totals | Compute write → GPU cull compute read |
| CullArgs | Device candidate totals and failure bits | Per-view \`VkDispatchIndirectCommand\` | Compute write → dispatch-indirect read |
| InstanceCull | Device ranges, leaf/member/instance tables, view | Verdicts | Compute write → compact compute read |
| Compact + LOD/work + drawbuild | Verdicts, count/LOD/geometry rows | Visible work and compact commands/counts | Compute write → draw-indirect/vertex read |
| Render Main/CSM | Commands, counts, work, view, geometry | Depth/color/atlas | Indirect/vertex read barriers and normal attachments |

Use real rendergraph logical resource edges, **not merely intra-command-buffer global memory barriers**. An indirect-dispatch buffer needs a compute-write-to-**draw/dispatch indirect command read** dependency. If a level becomes empty, choose a defined zero-work dispatch policy supported by the target API/driver; no stale arguments should be consumed. Separate compute/graphics queues require appropriate ownership/queue synchronization.

### Avoiding the giant fixed indirect-bin cost

Tourmaline's raster ABI has eight pipeline classes and a 20 B \`VkDrawIndexedIndirectCommand\`. The current compatibility drawbuilder pads each class to candidate capacity, potentially writing ~160 MiB for a 1M-candidate view. A staged replacement:

1. Count eligible commands by class on GPU; reject invalid/dead/legacy-owned rows before counting.
2. Prefix class counts; scatter **only nonzero** commands into a compact command buffer (or one contiguous segment per class).
3. Use \`DrawIndexedIndirectCount\` with GPU-generated counts, or another proven Vulkan-supported counted-draw path. Keep PSO-class-specific bind/dispatch sequencing.
4. Preserve generation identity and a work-row mapping: compacted command index cannot be assumed to equal the original work index. \`firstInstance\` must still point to the correct validated GPUScene work row or a new explicit compact-to-work map.
5. Check actual GPU limits (\`maxDrawIndirectCount\`, buffer-size limits, descriptor access) and split large batches into valid bounded submissions, rather than assuming 1M draws can be issued in a single counted draw.
6. Maintain a fully correct **GPU-side** overflow/fallback path. No "truncate candidates at 1,048,576", no hidden GPU→CPU readback to request direct fallback mid-frame, and no double-draw ownership.

Compact output reduces both buffer bandwidth and memory residency when few candidates survive. It cannot eliminate a full-scene fallback's work/memory requirements: fallback capacity and page traversal must be designed independently.

### Conservative overflow ownership — explicit gating

A GPU traversal that overflows after the CPU decided to omit GPU-owned geometry from direct shadows is **incorrect** unless another GPU path can draw every eligible owned caster. Implement a per-view, GPU-resident status word plus two mutually exclusive draw paths:

- \`status = VALID\`: use compact visibility commands, disable conservative fallback commands.
- \`status = OVERFLOW/INVALID\`: disable compact visibility commands, enable a **complete, independently capacity-proven all-owned-instance fallback**, possibly in pages. It may be slow, but may not omit anything.
- \`status = EMPTY_VALID\`: both paths correctly emit zero eligible draws.
- CPU-side ownership and capacity admission happens before frame recording; do not rely on learning GPU overflow through a frame-stalling readback.

A fallback itself is not "complete" merely because it has a larger fixed buffer. Prove all admitted instances have a drawable path (multi-submesh and residency included), test every failure mode, and ensure no stale commands can leak after switching states. A simpler rollout is **validation-only GPU visibility**, leaving complete direct shadows active until fallback and exclusivity are proven.

### Milestones with measurable exit gates

**M0 — instrumentation + ABI:** obtain actual GPUScene instance/leaf counts and current per-view allocation sizes, plus CPU ticket/selection cost and GPU \`preC\` time. Implement immutable GPU visibility snapshot storage and layout tests. **No rendering cutover.**

**M1 — flat GPU sphere baseline:** upload stable leaf sphere bounds; GPU classify five views, scan/scatter GPU ranges, produce device counts, and compare readbacks (test mode only) against a reference that can admit extra candidates but **must not miss any actually visible instance**. Prove the safe overflow mechanism before enabling exclusive GPU ownership.

**M2 — wide GPU sphere tree:** add level frontiers and subtree accept/reject. Compare flat GPU against hierarchical GPU on \`bistro_grid32\`, small scenes, large interiors, thin architecture, and dynamic-heavy worlds. If the sphere tree worsens GPU time or candidate inflation, keep the flat path.

**M3 — integrate Main + CSM:** switch at the per-view RenderGraph boundary behind \`TOURMALINE_GPU_SCENE_VISIBILITY\` (proposed switch), preserve direct/legacy fallback and A/B with #371. Verify shadow caster offscreen parity, masked/dithered materials, unknown/skinned bounds, extreme origins, multi-flight and forced overflow.

**M4 — optimize preC and memory:** compact per-class draws, optionally share view-invariant geometry/LOD selection, and only then explore GPU dynamic tree rebuilding and rebalancing if measured refit costs justify complexity.

The success metric is **wall-clock reduction**, not merely fewer tested spheres. Report separately: CPU visibility preparation, GPU visibility traversal/compaction, GPU \`preC\`, GPU raster and full frame; report peak transient VRAM and all conservative candidate/fallback counts. Do not claim specific millisecond wins without measured results.
