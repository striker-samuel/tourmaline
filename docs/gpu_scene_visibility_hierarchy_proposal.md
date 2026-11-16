# Proposal: independent transform and visibility hierarchies for GPUScene

**Status:** design proposal (no runtime behavior change)  
**Target:** `bistro_grid32`, ordinary open worlds, interiors, and GPUScene shadow RenderViews  
**Related:** #371 (GPU-driven CSM), `docs/gpu_scene_hlod.md` (geometry/LOD selection; distinct from visibility)

## Decision requested

Keep the authored transform DAG and **keep static transform flattening**. Do not rebalance transform parents to optimize renderer culling. Add a *separate spatial visibility index* over stable GPUScene instance identities, supporting:

1. Cook/load-time, balanced, wide **BVH4** over static world-space bounds, with an optional BVH8 benchmark.
2. Top-level world partitioning using Tourmaline's existing **64 m XZ leaf cells** initially.
3. A separate refittable/loose hierarchy for dynamic instances, updated from `gpuSceneGlobalChanges`.
4. Breadth-first, level-batched **SIMD CPU traversal** for coarse visibility. A node can be outside (reject subtree), fully inside (accept whole subtree), or partially visible (descend).
5. **One multi-view traversal** with independent view masks for the main camera and the four directional cascades. Produce separate per-view candidate ranges for the existing GPU instance cull -> compaction -> work/LOD expansion -> indirect chains.
6. No change to GPUScene generational identity, per-flight ownership, shader selection, or the direct fallback policy until a new selector proves parity and acceptable cost.

The *transform hierarchy* means `Scene::hierarchy` and dirty transform levels; it expresses parent/child motion and authoring. The *visibility hierarchy* groups objects according to space and conservative bounds, regardless of their authored parent. Neither structure is a surrogate for the other.

## Motivation and observed performance

Five measured `bistro_grid32` frames per configuration (reported from the stress scene):

| Configuration | Avg frame | CPU | GPU | Shadow raster | Passes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Direct shadows + direct motion | 2280 ms | 1948 ms | 183 ms | 0.14 ms | 8.9 ms |
| GPUScene shadows + direct motion | 3090 ms | — | — | 2.98 ms | — |
| Direct shadows + GPUScene motion | 2230 ms | — | — | 0.14 ms | — |
| GPUScene shadows + GPUScene motion | 3020 ms | 2289 ms | 112 ms | 2.73 ms | 54 ms |

These numbers show that #371 is **not** a performance win in this CPU-bound scene. The reported +341 ms CPU increase is associated with multiple coarse candidate-selection passes; `preC` GPU preparation also increases to roughly 42 ms. The attribution is a **hypothesis** to isolate with instrumentation, not a proven exclusive cause. The new visibility hierarchy is designed to reduce repeated CPU spatial selection, not to pretend that the four GPU compute pipelines are free.

### What the current source does

- `engine/include/Engine/Scene/static_export.h`: static authored subtrees are emitted as world-space roots, eliminating runtime parent transform propagation. Retain this optimization.
- `engine/include/Engine/Scene/scene.h`: a separate linked parent/child/sibling DAG and `changedAtThisFrame[MAX_NODE_LEVEL]` maintain dynamic transform dependencies.
- `engine/include/Engine/Renderer/gpu_scene_leaf.h`: `LeafRegistry` owns a `std::map<LeafCell, LeafState>`; `LeafCell` is a 64 m XZ translation cell. Actual leaf bounds include member geometry beyond cell edges.
- `engine/include/Engine/Renderer/gpu_scene_candidates.h`: `SelectPassedInstanceRangesFrame` allocates a ticket-key array, sorts it to check duplicates, linearly visits **every** leaf ticket for each requested view, tests its AABB against planes, and builds a frame-arena `PassedInstanceRange` array. No parent BVH pruning.
- `engine/include/Engine/Renderer/gpu_scene_world.h`: `WorldSynchronizer` already consumes the deduplicated `gpuSceneGlobalChanges` journal and feeds changed/retired instances to `LeafRegistry`. Preserve this update boundary.
- `engine/src/Renderer/WorldRendererGpuSceneCascades.inl` on the shadow branch uses `SelectPassedInstanceRangesFrame` once per cascade. The proposal is a separate PR; integrate with #371 only after correctness tests.

Avoid conflating the **visibility BVH** described here with `docs/gpu_scene_hlod.md`'s **geometry HLOD tree**. The visibility tree decides *which instances might be seen*; the LOD tree decides *which resident geometry represents them*.

## Data model and ownership

Persistent identities remain `Handle<Instance>{slot,generation}`; never key visibility ownership on merged `World::NodeId` or a compacted draw index.

```cpp
// Conceptual CPU layout, NOT the final wire/GPU ABI.
struct VisibilityRef {
    uint32_t instanceSlot;
    uint32_t instanceGeneration;
};
struct VisibilityLeaf {
    Aabb worldBounds;              // conservative; invalid -> never cull
    uint32_t firstRef, refCount;   // contiguous immutable/static ref span
    uint32_t cellIndex;            // initial mapping to existing LeafRegistry
    uint32_t revision;             // debug publication validation
};
struct VisibilityNode4 {
    // four children in SoA layout for AVX2 load/compare/bitmasks
    float minX[4], minY[4], minZ[4];
    float maxX[4], maxY[4], maxZ[4];
    uint32_t childOrLeaf[4];
    uint32_t childFlags[4];
};
struct VisibilityLevel {
    uint32_t firstNode, nodeCount; // contiguous per-level frontier
};
```

**Static publication:** sort leaf-cell centroids by Morton code (or LBVH), form bounded-size batches, build a balanced BVH4, and optionally refine or rebuild with SAH if overlap is pathological. Publish in a contiguous, frame-safe snapshot. The 64 m cell is *not* assumed to be a tight axis-aligned cube: use the **published transformed mesh bounds**, which can cross cell boundaries and extend in Y. Preserve leaf identities and instance references from the existing GPU database.

**Dynamic publication:** keep moving instances in a separate loose BVH or cell index. A changed world transform recomputes its conservative world bound and refits ancestor unions; relocate only if it escapes its loose leaf. Static and dynamic trees yield the same candidate range interface. Unresolved/skinned/unknown bounds must be handled conservatively, with dedicated always-visible/fallback handling rather than incorrect clipping. Rebalance/rebuild asynchronously only at a safe publication boundary *if scheduling infrastructure supports it*; synchronous incremental refit is the baseline design.

**Do not** mix BVH topology writes into `Scene::hierarchy`, `changedAtThisFrame`, or the static export flattening process. Visibility nodes may move/rebalance without moving scene transforms or renumbering GPUScene instance slots.

## Traversal: levels, SIMD, inside/outside/partial

Use tightly packed SoA AABBs, 4 children per node as a baseline. At each frontier, test all children against all active view frusta using origin-relative coordinates. A child is:

- **Outside:** its conservative AABB is wholly outside at least one active view plane; clear that view's bit.
- **Fully inside:** contained by every plane; accept its *entire descendant range for that view*, without further per-node plane tests.
- **Partial:** propagate that view's bit to the next level.
- **Unknown/invalid bounds:** fail open (partial, or explicit uncullable range), never silently discard geometry.

```text
frontier = [{root, activeMask = main|c0|c1|c2|c3}]
for level in 0..maxLevel:
    parallel_for chunk in frontier:
        SIMD-test the node's 4 child AABBs against each active view
        for each child:
            for each view whose bit remains:
                outside -> nothing
                inside  -> append subtree's eligible leaf ranges to view output
                partial -> add child to next frontier with that view bit
    prefix-scan/join the next frontier
```

This is pseudocode: a node can be inside one view and partial in another. Do not accept a leaf for *all* views based on a single view result. Parallel output requires per-thread staging or deterministic prefix allocation; do not enqueue a task per node. Test BVH4 versus BVH8 with the same scenes, depth histograms and output identity checks before selecting a permanent ABI.

An entirely accepted subtree needs a compact, contiguous leaf/ref-range representation (or a subtree range table) to avoid expanding every internal leaf back into O(N) CPU work. If a subtree spans disconnected current `LeafMember` ranges, encode a small range list or introduce a packed visibility-ref stream; never assume arbitrary leaf IDs map to one contiguous GPUScene range.

## Multi-view / directional-shadow correctness

Use five view bits initially (main, C0, C1, C2, C3), leaving room for spot, point and custom RenderViews. Each node carries an **active/inside mask** independently for each view. Child propagation removes only views that reject the child; a camera-rejected child may still be accepted by a cascade.

Directional shadows must test the actual light-space cascade caster volume, and retain off-camera casters that can contribute to the visible receiver region. **Never** use the main-camera frustum as a prefilter for shadow candidate ranges. Optional swept-contribution pruning is allowed only if its mathematical conservativeness is proven and tested; otherwise use the full cascade/light caster bounds.

Each view produces independently prefixed `PassedInstanceRange` data referencing shared immutable member rows and its own view cull descriptor. The GPU cull then applies that view's six planes and `CastShadow` bit as required. Work/LOD expansion and indirect commands remain independent for now; a later GPU optimization can amortize residency and submesh selection where semantically equivalent.

**Capacity:** `65536` ranges and roughly `1,048,560` cascade candidates are current caps, not a safe budget to enforce by truncation. If any view overflows, or any bounds/snapshot fail validation, **fall back that view completely** to the proven direct path. No partial owned GPU set combined with accidentally omitted legacy geometry. Empty views are distinct from invalid views.

**Lifetime:** publish an immutable BVH + ticket/member snapshot per acquired flight. Ownership/generation and logical RenderGraph dependencies must remain valid until all consumers have completed. Do not reuse scratch across concurrently active view dispatches; do not let frame-arena pointers escape their frame.

## Proposed incremental delivery (each milestone separately testable)

### Phase 0 — measurements and selection hygiene

1. Add timing around `LeafTicketsFrame`, ticket duplicate verification, frustum AABB tests, prefix-range output, GPU upload, CPU direct fallback filtering, and the four independent GPU compute chains.
2. Move duplicate ticket identity validation to leaf publication/topology revision; cache a validated immutable ticket snapshot. Still reject invalid snapshots at the reader boundary, without O(n log n) sorting per view.
3. Implement **one-pass multi-view flat leaf scan** (no BVH yet), emitting separate candidate streams, to isolate repeated traversal from tree-build benefits.
4. A/B select the existing scalar, SIMD flat, and multi-view flat paths behind debug flags.

### Phase 1 — static visibility BVH

1. Build BVH4 from existing conservative 64 m `LeafRegistry` leaf bounds; preserve handles and membership ranges.
2. Level-batched/AVX2 traversal with inside/partial/outside classifications, conservative frustum epsilon and fail-open invalid bounds.
3. Validate against the current `SelectPassedInstanceRangesFrame` CPU oracle for multiple camera/cascade frusta, reversed-Z, near-plane crossings and very large camera origins.
4. Integrate the multi-view selector with main + CSM, keeping the old selector as a fallback until parity.

### Phase 2 — incremental updates

1. Split static and dynamic index storage.
2. Feed the existing dirty-transform journal to bound refit/relocation, and mesh-residency/bounds changes to safe refresh.
3. Refit only touched ancestors; bound amortized relocation; rebuild/rebalance only when a measurable quality metric warrants it (overlap, depth, refit count).
4. Test rapid spawn/despawn, slot reuse, generation tombstones, cell crossings, nonuniform/negative scale, skinned/unknown bounds and scene reload.

### Phase 3 — GPU work reduction

1. Profile counts and `preC` in each cascade with the new selector. A faster CPU hierarchy alone may still create four costly GPU cull/work/indirect pipelines.
2. Explore a shared resident-LOD/submesh expansion or batched multi-view candidate verdict stage **only where view-specific visibility, shadow caster flags, and LOD semantics permit it**.
3. Compare BVH4, BVH8, and flat SIMD, plus GPU-side traversal if CPU load remains material. Do not mandate GPU traversal without evidence.

## Instrumentation / acceptance

Capture for every run:

- Total nodes, static instances, dynamic instances, number of spatial leaves and BVH nodes, depth distribution, mean/max child overlap.
- Build time, changed-instance count, refit time, rebuild time, scratch bytes, peak publication memory.
- `LeafTicketsFrame` time, identity validation time, coarse cull CPU time (per view and total), candidate ranges/instances (per view), fully accepted/pruned/partial subtree counts.
- GPU `preC`, CSM raster, Main, motion, memory/indirect count, full frame CPU/GPU/wall time; distinguish shader work from host command recording.
- Overflow/invalid-view counts and fallback fraction, **not** just elapsed time.
- Per-view *identity set* parity to the old conservative selector; GPU raster parity including off-camera directional casters, masked/dithered materials and frame-to-frame motion.

Success means no newly missing objects/shadows and no partial-overflow omissions; unchanged GPUScene ownership/LOD identity; no regression on small/dynamic scenes; and a **measured reduction in `bistro_grid32` coarse-selection CPU time** relative to #371. A stretch objective is to recover the reported ~341 ms CPU regression, but do not make that an unverified promise. The ~42 ms GPU cascade preparation is a separately tracked bottleneck.

## Non-goals

- Reparenting, balancing, or unflattening the authored transform hierarchy.
- Replacing GPUScene instances, generational handles, the material/LOD system, or the existing shading/RenderGraph contracts.
- Automatically creating visibility proxies or HLOD meshes.
- Using camera visibility as a substitute for cascade caster visibility.
- Merging this design alone as evidence of a working BVH or improved runtime performance.

## Review questions

1. Should the first prototype stop at shared multi-view flat SIMD selection before introducing BVH topology? **Recommended yes**; it gives a clean baseline for why a tree helps.
2. Should we retain 64 m translation cells as the initial BVH leaves? **Recommended yes**, then split pathologically populous/overlapping cells based on measurements.
3. Is a separate dynamic loose BVH preferable to refitting the static index? **Recommended yes** for high churn.
4. Should #371 depend on this redesign? **No hard dependency** until parity is demonstrated; land the new selector behind a switch and migrate CSM deliberately.
