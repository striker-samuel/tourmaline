# RFC: Static compiled render graph

Status: architecture decisions supplied by maintainer; implementation pending. This PR is documentation only, not a runtime change.

## Accepted constraints

One monolithic compiled graph per structural permutation; no independently compiled subgraphs. Explicit async submit sections from the first compiled executor. Clean replacement of the existing RenderGraph, retaining familiar API naming and builder style where practical; update all affected callers in the same integration change. Cached compiled permutations and fixed topology for dynamic work. Whole-resource synchronization initially. No split barriers.

## Goal

Compile persistent graph variants at initialization or explicit structural changes. On a cache hit, frame execution must not build pass declarations, collect usages, sort dependencies, pack heaps, or analyze interior barriers. Record fresh GPU commands using current frame data. No split barriers (no Vulkan events); use synchronization2 full barriers at batch boundaries. Preserve logical insertion order for conflicting resource accesses; independent work may move.

## Current baseline

- engine/include/Engine/Rendergraph/rendergraph.h: logical pools, RenderPass, callbacks, NewFrame clears passes only.
- engine/src/Vulkan/impl.cpp: Execute resolves resources, collects physical usages, stable topological sort, compiles transient heap, resolves views, snapshots states, emits barriers and records. Stable min-index sort with forward-only edges preserves insertion order.
- WorldRenderer and feature modules build frame-local passes and callbacks, including ForwardPlusRenderer. Existing lambdas may capture frame-local values and cannot simply be retained.
- Current EVSM async split uses dedicated compute where available; header comments lag implementation. Existing transient heap shares shape-compatible image allocations by pass intervals; this is insufficient for reordered/overlapping batches without revised lifetime rules.

## Proposed architecture

Monolithic RenderGraph declaration -> compiler -> immutable CompiledRenderGraph -> executor. Feature helpers append declarations to the same graph; no independently compiled or executed subgraphs. Flatten into stable IDs and contiguous arrays; timelines encode the DAG. One plan owns all resources, batches and submit sections for a permutation.

Suggested types (illustrative):
```cpp
struct CompiledGraph {
    GraphKey key;
    vector<CompiledPass> passes;
    vector<Batch> batches;
    vector<BarrierTemplate> barriers;
    vector<ResourcePlacement> placements;
    vector<BindingPatch> patches;
    vector<SubmitSection> sections;
};
using RecordFn = void(*)(CommandBuffer&, const FrameContext&, const PassData&);
```
PassData is immutable owned configuration; frame-dependent data lives in typed FrameContext or per-flight binding tables. Never retain references to a previous frame's locals. Stable IDs and names are compile-time data. Retain familiar AddImage, AddExternalImage, AddPass, AddComputePass, SetColor, SetDepth and hint-style names where practical, but replace their implementation and contracts directly. No compatibility adapter or permanent legacy execution path. Compile explicitly; Execute accepts current frame data and bindings. Remove NewFrame pass-list reconstruction and update all affected callers atomically at integration. Explicit side-effect/fence declarations for uploads, queries, readbacks, presentation, global bindless/BDA resources and opaque callbacks. Unknown usage pins a pass or section conservatively; do not silently reorder.

## Compiler

1. Flatten monolithic declarations; validate IDs, descriptors, imports, pass access types and side effects. Normalize by resource identity (including multiple logical aliases) at whole-resource granularity. Reject conflicting same-pass layouts unless explicitly supported. Views retain their ranges for rendering, but all views of the same image share one synchronization timeline initially. Reject unsupported conflicting layouts rather than pretending ranges are independent; add finer tracking in a later change.
2. Construct resource timelines in logical recording order. Each ordinary write is a singleton; consecutive reads form groups with unioned access/stage masks. A writer waits for all readers since the previous write; readers wait for prior writer. Optional concurrent writes require explicit proven disjoint/atomic semantics, not ordinary writes. Preserve prior-write visibility and all reader stages. Layout-incompatible reads initially split into ordered groups; enable compatible common layout (including GENERAL only when all commands/descriptors support it) after validation and profiling. Depth read-only and indirect command fetch need precise access.
3. Schedule each pass at earliest legal dependency level: batch = max(predecessor batch + 1), or zero. Groups track min/max assigned batch; a later writer waits on max. Read group transition is before min. No explicit adjacency required for primary algorithm; retain debug dependency export. Preserve order for side effects and submit fences. Deterministic stable tie breaks. Initially preserve order inside batches; later sort by graphics/compute/transfer only if measured and safe.
4. Optional backwards slack sweep: move noncritical passes later without increasing dependency depth, only if resource byte-batch area decreases (or measured queue-kind policy improves). Recompute group min/max and intervals. Disabled initially.
5. Allocate transient memory from final inclusive batch lifetimes, never alias images within same batch. Keep existing shape-compatible image packer initially, then evaluate longest-lifetime-first aligned 2D packing with memory type, alignment, dedicated-allocation, alias flag and buffer/image granularity restrictions. Explicit alias memory dependencies between old and new occupant, discard/initialize new occupant, never assume UNDEFINED layout alone orders prior writes. Exclude history, imports, read-first resources and overlapping queue lifetimes. Per-flight heaps or proven inter-frame reuse synchronization; retire old graph/resources after last GPU use. Do not reuse allocations across live variants unsafely.
6. Emit ordinary VkDependencyInfo templates per batch boundary: RAW/WAR/WAW, layout transitions, alias handoffs, correct unioned stage/access masks. No split barriers. Batch 0 imports/first use may need a barrier too. Incompatible layouts or dependencies that cannot be expressed at one boundary require separate batches. No blanket ALL_COMMANDS except conservative fallback. Store stable resource references, not frame-specific handles. Compile dynamic rendering attachment templates and pipeline signatures.
7. Validate and publish immutable plan; dump resource timelines, batches, barriers, lifetimes, memory offsets, queue sections and cache key.

## Dynamic resources and variants

GraphKey contains only structural inputs: renderer route, extent/format/sample count, feature toggles, view topology, capacity/descriptor contracts, queue policy, shader interface generation and other declarations that affect usages. Never key on camera matrices, draw counts or swapchain image index. A dynamic slot carries expected resource shape, alias class, required initial/final state, and per-flight actual handle. Reject unexpected physical aliasing between slots or recompile conservatively. Patch only affected barriers, attachment handles, descriptors, addresses and constants. External first-use barriers depend on actual imported state and upload semaphore dependencies; final state must be committed back to resource manager. Swapchain acquire/present stays explicit. Cross-frame history and ping-pong need correct flight synchronization and state contracts. Resize/reload/settings change selects or builds a variant at an explicit safe boundary. Cache bounded by memory budget, with GPU-safe retirement; avoid uncontrolled combinatorial permutations. Prewarm common variants. No compile in normal Execute; define policy for missing variants (old compatible graph or explicit transition, never stale incompatible bindings).

Optional work: prefer fixed topology with valid empty dispatch/draw or neutral producer output; skipping a producer cannot leave undefined data for consumers. Truly different accesses require cached variants, not conditional barrier omissions. Shadow tiles, probe updates, debug passes and editor view counts need an audit; bounded maximum slots or explicit structural variants. Dynamic resource list changes that change hazards cannot be treated as simple rebinding.

## Queues

Explicit submit sections are mandatory from the first compiled executor. Each section has explicitly assigned graphics/compute passes and an authored submit boundary; do not reorder across sections. Validate that each resource is used by only one queue within a section (including physical aliases); reject conflicting declarations. Cross-section dependencies compile into timeline semaphore signal/wait pairs with appropriate stage scopes. Cross-queue resources use concurrent sharing initially; no implicit ownership transfer. On hardware without a dedicated compute queue, lower to one graphics queue with the same section order and full barriers. Preserve EVSM behavior through authored sections. Use queue-local batches and prohibit aliasing across concurrently executing streams unless a compiled happens-before proof exists. Full barriers are distinct from semaphore submission dependencies. No automatic queue assignment or split barriers.

## Migration and review checkpoints

A. CPU-only compiler and tests: whole-resource timelines, access groups, explicit section and queue validation, deterministic dependency levels, barrier templates and lifetime analysis. No runtime change in this preparatory commit.
B. Implement compiled Vulkan execution with explicit async sections, per-flight binding and imported boundary states, timeline semaphores, single-queue lowering, full barriers and graph dump. Develop against compiler tests and samples; no independently shipped second rendergraph API.
C. Atomic integration: replace existing RenderGraph implementation and migrate every caller (WorldRenderer, feature modules, editor, samples, tests). Preserve familiar builder names where sensible, replace per-frame captures with stable record functions and typed data, remove NewFrame-driven rebuilding. Audit all bindless/BDA access, opaque callbacks and side effects. No legacy compatibility adapter or permanent dual executor. Keep intermediate development commits buildable; if necessary combine B/C into one integration commit.
D. Enable ASAP reordering and compatible read groups with correctness tests, reuse existing image packer with batch lifetimes and explicit alias handoffs. Compare against pre-change baseline using an external baseline build, not a second shipped executor.
E. Cache bounded structural permutations, prewarm at settings/level transitions, exercise resize, reload, capacity and history reset. Optional slack optimization, subresource tracking and buffer aliasing are subsequent independent changes.
F. Validate and benchmark before merge. No runtime feature may be silently lost; explicit async EVSM and single-queue fallback are required.

## Acceptance

CPU tests deterministic and device-independent; existing test_rendergraph_contract passes. GPU sync validation clean, no device loss on graph fuzz; representative golden captures match within established tolerances. Test two+ frames in flight, resize, feature toggles, dynamic import alias rejection, async fallback and deferred retirement. Report cold compile cost, cache hit rate, per-frame CPU build/record time, batch count/barrier calls, heap peak/aliased bytes and GPU timings on same scenes/settings/hardware against baseline. No promised performance gain without measurements. Cache hit instrumentation must show zero graph builds/sorts/heap packs and no interior hazard analysis; only patch imported boundary states and record. Preserve debug profiling/checkpoints and hot reload.

## Maintainer decisions (resolved)

- Monolithic static graph, one compiled plan per structural permutation.
- Explicit async submit sections from day one.
- Clean replacement, familiar API where practical, breaking changes permitted with caller updates in the same integration commit.
- Cached compiled permutations plus fixed topology for dynamic work.
- Whole-resource tracking first; extend to subresources later.

This RFC is a documentation review checkpoint; the implementation must be separately reviewed before merge.
