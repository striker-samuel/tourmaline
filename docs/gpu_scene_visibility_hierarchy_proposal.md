# RFC: separate visibility hierarchy — balanced sphere tree and multi-view SIMD

**Status:** design-only RFC; no renderer or ABI changes in this PR.  
**Related:** #371 (GPUScene cascade shadows); `docs/gpu_scene_hlod.md` (geometry LOD hierarchy, a separate concern).  
**Design constraint:** **Do not use axis-aligned bounding boxes for visibility nodes or traversal.** A balanced tree is desirable; a BVH is an optional implementation strategy, not the objective.

## Decision

**Keep static transform flattening.** Tourmaline's authoring/transform tree describes dependencies; a visibility tree must describe spatial coherence. Never rebalance or reparent the author's Scene just to make rendering faster.

Introduce a **separate, balanced, wide hierarchy of conservative bounding spheres**, referencing the existing stable GPUScene instance rows by `{slot, generation}`. The spatial hierarchy has its own update rules, memory, topology and publication epoch. Its primary job is to select *groups* of spatially coherent instances for the existing GPU compute pipeline, including all directional cascade views in one traversal. It does not change material, shading, LOD or instance identity.

A sphere is a **design choice to prototype and measure**, not a claim of universally superior culling. It has rotation-invariant tests and fits well with SIMD but can be loose around long/thin objects and large, irregular groups. If its measured rejection quality fails, investigate tighter **non-axis-aligned** volumes (e.g. oriented capsules for long thin groups, oriented boxes or k-DOPs) before changing the architecture. Do not fall back to AABB by default.

### Immediate plan

1. First remove repeated ticket snapshots, per-view duplicate sorting and flat-leaf rescanning with one **multi-view flat sphere selector**, preserving the current GPU candidate ABI.
2. Then add the balanced sphere tree, breadth-first/level-batched SIMD traversal and whole-subtree pruning; measure its extra benefit rather than assuming it.
3. Separate immutable/static and dynamic/refittable spatial trees only when update metrics justify the additional complexity.
4. Independently optimize the ~42 ms GPU cascade preC stages; the CPU visibility tree will **not** eliminate four GPU work/LOD/indirect pipelines.

## Evidence from Tourmaline

- `engine/include/Engine/Scene/static_export.h` flattens static authored subtrees into independently transformed roots. This is deliberate and remains intact.
- `engine/include/Engine/Scene/scene.h` has `Hierarchy`, `changedAtThisFrame[MAX_NODE_LEVEL]` and `gpuSceneGlobalChanges`. Those belong to transform propagation, **not** spatial visibility traversal.
- `engine/include/Engine/Renderer/gpu_scene.h` already publishes an **instance `localSphere`** plus world translation and basis. It also has a `Leaf` with `worldCenter + halfExtents`; that is an existing **AABB-based ABI**, *not* the proposed visibility tree's data model. Changing this existing GPU cull ABI is a separate migration decision.
- `engine/include/Engine/Renderer/gpu_scene_leaf.h` puts instances in 64 m XZ **translation cells** and publishes conservative bounds; the cell grid can remain a *partition key* but must not become a visibility AABB.
- `engine/include/Engine/Renderer/gpu_scene_candidates.h` sorts all leaf keys to check duplicates **on every call**, scans all leaf tickets per view and emits `PassedInstanceRange`. Those repeated operations are a more immediate target than tree balancing.
- `engine/include/Engine/Renderer/gpu_scene_world.h` already processes `gpuSceneGlobalChanges` into changed instance rows and leaf registry upserts; reuse the journal without reintroducing transform-parent dependencies.

### Stress-scene motivation

Reported `bistro_grid32` (five frames per configuration):

| Configuration | Frame avg | CPU | GPU | Shadow raster | Total passes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Both direct | 2280 ms | 1948 ms | 183 ms | 0.14 ms | 8.9 ms |
| GPU shadows / direct motion | 3090 ms | — | — | 2.98 ms | — |
| Direct shadows / GPU motion | 2230 ms | — | — | 0.14 ms | — |
| Both GPU | 3020 ms | 2289 ms | 112 ms | 2.73 ms | 54 ms |

The +341 ms CPU delta and ~42 ms of `preC` preparation motivate profiling; repeated leaf selection is a plausible contributor, but **not an isolated measurement**. The experiment must attribute costs to ticket snapshot, key verification, spatial selection, candidate serialization, GPU uploads, legacy shadow fallback and GPU passes separately.

## Structure: logical spatial hierarchy, sphere-only visibility tests

### Separate concerns

```text
Scene transform DAG (authored)                   Visibility hierarchy (derived)
    static roots: baked world transforms           balanced sphere tree of static objects
    dynamic nodes: parent/child updates            dynamic loose sphere tree / overflow list
                 |                                           |
                 +--> GPUScene instance {slot,generation} <--+
                                    |
                         independent per-view GPU work
```

Instance identity is generational and stable; tree node indices are **not** renderer draw IDs. Visibility topology may be rebuilt/rebalanced without touching `Scene::hierarchy` or reauthoring transforms. Dynamic moved instances only update their own sphere and ancestors in the visibility tree.

### Prototype records (NOT committed GPU/HLSL ABI)

```cpp
struct SphereD {
    glm::dvec3 centerWorld;     // double-precision authority
    double radius;              // conservative, finite, nonnegative
};
struct VisibilityRef {
    uint32_t instanceSlot, instanceGeneration;
};
struct SphereNode4 {
    // One level-local compact child block; SoA enables SIMD on four
    // sibling spheres. Float coordinates are relative to a double
    // origin local to a region, with an explicit outward rounding guard.
    float cx[4], cy[4], cz[4], radius[4];
    uint32_t childOrLeaf[4];
    uint8_t childType[4];       // internal / leaf / invalid
    // Remaining fields decided after alignment + cache profiling.
};
struct VisibilityLeaf {
    uint32_t firstRef, count;   // contiguous refs, generational
    uint32_t flags;             // complete bounds / fail-open / dynamic
};
struct VisibilityLevel {
    uint32_t firstNode, count;  // packed breadth-first frontier
};
```

**Important ABI bridge:** the existing `PassedInstanceRange` is 32 bytes containing `LeafMember` start/count, *candidate prefix* and a generational `Leaf` ID. The GPU cull shader verifies both. The first implementation must **emit that existing layout** and preserve leaf identity; do not assume packed BVH child indices are valid `Leaf` IDs or arbitrary descendant ranges can be collapsed into one GPU candidate range. Accepting a whole internal subtree may skip further *spatial tests*, but unless its leaf/member rows are physically adjacent it must still enumerate/emits its descendant leaf ranges.

The future packed-ref layout could allow one contiguous subtree range, but that requires a **versioned GPU cull shader and upload ABI migration**, not an undocumented reinterpretation of existing member rows.

### Sphere bounds derived from existing instances

For local center `c`, radius `r`, world translation `T`, and linear basis `B`:

```text
centerWorld = T + B*c
radiusWorld >= r * sigma_max(B)
```

The radius must be conservative under nonuniform scale and shear. A cheap proven upper bound is `sqrt(max_i sum_j |(BᵀB)[i,j]|)`; avoid using `max(length(B.column))`, which is **not** always conservative under shear. Evaluate exact/stronger bounds at cook time if loose spheres inflate candidates. Use double-precision centers when building and refitting, then outward-round sphere radii when publishing to float relative to a double anchor. Nonfinite/invalid geometry and unknown skinned bounds are **uncullable**, never assumed empty.

Combine two child spheres `(c1,r1)` and `(c2,r2)` conservatively: if one contains the other, retain the larger. Otherwise, for `d=|c2-c1|`, use `r=(d+r1+r2)/2` and `c=c1 + (r-r1)/d * (c2-c1)`. Widen the stored radius to absorb numerical error and packed-float quantization. A naive mean-center/maximum child-radius can miss descendants and is forbidden.

### Balancing policy

**Start with BVH4 as a storage/traversal shape, not a strict binary/AVL-style balancing algorithm.** The primary quality metrics are **total conservative volume/radius inflation, sibling overlap, leaf occupancy, traversal nodes visited and candidate count**; tree depth is secondary. A perfectly depth-balanced but highly overlapping sphere tree can be slower than a shallower, less regular tree.

Static build: group by spatial locality (initially the 64 m translation-cell keys, then sphere-center Morton order), split large populated groups, and construct a bounded-fanout bottom-up sphere hierarchy. Compare centroid clustering, spatial median and greedy volume-increase insertion/refinement on measured scenes. Avoid a full sort/rebuild on every render frame. Preserve deterministic construction and stable leaf identities across rebuilds.

Dynamic build: choose **a separate loose sphere tree** or a sparse dynamic cell registry so routine motion only changes local bounds. Refit ancestors bottom-up on a dirty-node set; relocate only when containment fails. Trigger bounded rebalancing when overlap/radius inflation or movement frequency exceeds a measured threshold, not every time a transform changes. Rebuild static topology only for actual topology/geometry changes.

Very elongated architecture can produce poor parent spheres: benchmark whether a few separately grouped clusters or non-axis-aligned oriented volumes outperform a single huge sphere before adding a new per-instance test.

## Visibility traversal: full/partial/outside, per-view

Let a sphere center in a view-relative frame be `C`, radius `r`, and an inward plane be `(n,w)` with `dot(n,p) + w >= 0`. Use `d = dot(n,C) + w` and `extent = r*|n|`:

- **Outside:** `d + extent < -epsilon` for **any** plane. Reject for that view.
- **Fully inside:** `d - extent >= epsilon` for **all** planes. All descendant bounds are inside that view; the frustum tests are no longer needed for this subtree.
- **Partial:** otherwise descend; keep that view's bit live.
- Nonfinite/unknown bounds: treat as partial / fail-open, not outside.

`epsilon` is a conservative, scale-aware numerical guard for the high/low origin split and float packing. Avoid blindly applying the main camera's near/far convention to directional cascade planes. Retain the current validated `RelativeFrustum` and the same per-view origin until sphere parity passes.

### Breadth-first SIMD pass

A bounded frontier holds a node index, `activeViews` bitmask and `insideViews` bitmask. Process level-contiguous nodes in chunks; test 4 sibling spheres at once with SIMD (AVX2 baseline, scalar fallback). For each child and view, update the masks:

```text
outside that view -> clear view bit
inside that view  -> set inside bit (no further plane tests for that view)
partial           -> retain active bit for the next level
no active views   -> prune subtree
leaf             -> append leaf reference to each relevant view output
```

Do **not** treat an entire cascade's visible candidate list as the camera-visible list. Every light view remains independently conservative. If a node is inside C0 but partial in C1, C0 propagates as inside while C1 is tested at deeper levels. Accepting the subtree still needs existing leaf/member emission until the range ABI is redesigned.

Avoid global atomics per node or a task per node. Use per-worker frontier chunks and two-pass counts/prefix allocation where needed; compare deterministic and unordered scheduling on the same input. Ensure the widest-first walk's scratch remains bounded and uses frame-lifetime ownership.

### Why multi-view should be measured, not assumed

A fused traversal amortizes node **loads** and tree walks; it does **not** necessarily amortize six plane tests per view. Traversal may be worse if all five views are spatially unrelated and the combined frontier remains broad. Implement both **flat one-pass multi-view** and **BVH multi-view**, plus separate-view traversal; benchmark the three with the *same* candidate identities and report CPU time, node loads, SIMD utilization and output sizes.

A useful extension is grouping cascades by overlap or using a shared coarse sun-volume with per-cascade refinement; only enable it if the enclosing test conservatively contains all required casters. No camera-frustum prefilter for shadows.

## Integration boundaries and hard safety contracts

1. **Current culling ABI remains unchanged initially.** The sphere tree produces the existing `FrameCandidateSelection`: valid `PassedInstanceRange` and per-view `candidateBase` prefixes, not a novel GPU layout. The downstream GPU culling kernel continues precise instance culling and generation/owner checks.
2. **No false negatives.** Spatial bounds must cover every draw-reachable LOD, dynamic/skinned deformations and all relevant shadow casters. Unknown coverage is fail-open. For cascades, the visibility volume must contain *contributing off-camera casters*, not just the receiver camera frustum.
3. **Per-view capacity is atomic.** An invalid hierarchy/view, stale epoch, duplicate ref, failed allocation or `MaxPassedLeafRanges` / `MaxPassedInstanceCandidates` overflow means that **entire view** uses the legacy fallback. No truncation to hit a speed target.
4. **Publication is flight-safe.** Rebuild a derived snapshot from the authoritative registry; validate acyclicity, child range extent, identity uniqueness, conservative containment, node count, level offsets and resident handles; publish only once ready. Retain old snapshots until GPU consumers retire. Tree traversal never reads an in-progress refit.
5. **Static and dynamic entries must not double-publish.** Transfer between indexes at the visibility publication boundary using stable instance identity. The GPU scene generation mask, not spatial membership, is the final draw ownership authority.
6. **Shadow-specific filters stay in downstream GPU culling.** Hierarchy tests operate on conservative geometry extents; `CastShadow`, dither, masked materials, LOD selection and PSO compaction retain their existing semantic owners.

## Delivery stages (one measurable change at a time)

### Stage A — measurement and safe hot-path cleanup

- Instrument **ticket generation**, duplicate validation/sorting, candidate selection, range copying/upload, fallback walk and `preC` per cascade.
- Cache a validated topology epoch / ticket snapshot and eliminate repeat identity sorting *only after* a publication-time uniqueness proof. Stale/corrupt snapshots still reject completely.
- Implement a **single flat multi-view sphere test pass**, retaining existing leaf member ranges and GPU cull output. Compare against current flat box-leaf selection for *superset safety*, not exact accepted-count equality: different conservative volumes legitimately admit different extra instances.
- If Stage A already removes the CPU hotspot, delay the tree.

### Stage B — packed wide sphere tree

- Build static sphere nodes over existing spatial leaf identities.
- Implement level-batched scalar and SIMD4 traversal with three-state tests and per-view masks.
- Add full-subtree acceptance plus safe leaf enumeration, with max-output preflight or rollback that prevents partial publication on failure.
- Benchmark BVH4 vs alternative fanout and vs flat SIMD; integrate only if measured wins justify memory/build overhead.

### Stage C — dynamic updates

- Add separate dynamic loose-sphere structure from `gpuSceneGlobalChanges`, geometry updates and generational removal.
- Implement bottom-up dirty-only refit, bounded relocation and periodic quality-based balancing.
- Validate spawn/despawn, edits, slot reuse, huge origins, nonuniform transforms, geometry residency, skinned/morph bounds and invalid numerical input.

### Stage D — GPU work improvements

- Compare GPU cascade `preC` times and candidate counts after the new CPU selector.
- Prototype shared residency/LOD evaluation **only if equivalent for those views**; preserve separate view frustum and caster masks.
- Evaluate GPU-side tree traversal if the CPU selector remains a bottleneck after flat multi-view and SIMD sphere traversal are measured.

## Validation matrix and acceptance

Record frame time, CPU render-thread time, per-view candidate selection and serialization time, GPU `preC`, shadow raster, Main raster and memory. Track node/leaf counts, max frontier size, hierarchy depth, sibling sphere overlap/inflation, candidate count, percentage full-inside/pruned/partial, and fallback reasons. Include small/mobile scenes where a tree is likely overhead.

Correctness tests:

- Every current oracle-visible instance must remain a candidate (**set inclusion**), independently for Main and each cascade. The new selector can return conservative **extra** candidates; it may never omit an oracle-visible one.
- Random seeded scene sets and 32x32 repeated grid; independent/crossing/shadow-only frusta; camera cuts; near-plane and infinite-far projections.
- Spheres under translation, rotation, negative scale, nonuniform scale, shear, 1e12 world origins, large structures spanning cell borders, tight thin geometry and invalid/unknown bounds.
- Tree construction, parent-sphere containment, every unique live identity exactly once, static/dynamic transfer, refit/relocation, retirement/generation reuse, stale frame snapshot and capacity overflow.
- End-to-end masked/dither/skinned shadow parity, DXC/Vulkan validation layers, frame-flight reuse and draw ownership. Verify no missing off-camera casters.

**Performance criteria:** demonstrate a statistically meaningful **wall-clock CPU coarse-selection improvement** on `bistro_grid32` relative to #371 while maintaining render parity and bounded publication cost. Quantify tree build/refit overhead and candidate inflation, rather than claiming sphere culling is always faster. Track the approximately 42 ms GPU preparation cost as a separate target.

## Explicit non-goals

- Returning the static transform scene to a deep runtime DAG.
- Replacing the existing GPUScene `Leaf` ABI or HLSL in the RFC stage.
- Using **axis-aligned bounding boxes** for the new hierarchy or its visibility tests.
- Claiming that balanced depth alone makes a visibility tree good.
- Reusing the main-view visible set as the directional caster set.
- Treating this design-only PR as implemented or benchmarked.
