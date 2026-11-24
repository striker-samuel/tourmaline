# RFC: Static compiled render graph

Status: proposed; review required. This PR is documentation only, not a runtime change.

## Goal

Compile persistent graph variants at initialization or explicit structural changes. On a cache hit, frame execution must not build pass declarations, collect usages, sort dependencies, pack heaps, or analyze interior barriers. Record fresh GPU commands using current frame data. No split barriers (no Vulkan events); use synchronization2 full barriers at batch boundaries. Preserve logical insertion order for conflicting resource accesses; independent work may move.

## Current baseline

- engine/include/Engine/Rendergraph/rendergraph.h: logical pools, RenderPass, callbacks, NewFrame clears passes only.
- engine/src/Vulkan/impl.cpp: Execute resolves resources, collects physical usages, stable topological sort, compiles transient heap, resolves views, snapshots states, emits barriers and records. Stable min-index sort with forward-only edges preserves insertion order.
- WorldRenderer and feature modules build frame-local passes and callbacks, including ForwardPlusRenderer. Existing lambdas may capture frame-local values and cannot simply be retained.
- Current EVSM async split uses dedicated compute where available; header comments lag implementation. Existing transient heap shares shape-compatible image allocations by pass intervals; this is insufficient for reordered/overlapping batches without revised lifetime rules.

## Proposed architecture

Feature tree -> GraphBuilder -> GraphCompiler -> immutable CompiledRenderGraph -> Executor.
A feature tree is a compositional declaration hierarchy, not a tree restriction on the dependency graph. Flatten it into stable IDs and contiguous arrays; resource timelines encode the DAG. No tree traversal on steady-state execution.

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
PassData is immutable owned configuration; frame-dependent data lives in typed FrameContext or per-flight binding tables. Never retain references to a previous frame's locals. Stable IDs and names are compile-time data. Keep existing pass builder sugar as a migration adapter, with precise access declarations underneath. Explicit side-effect/fence declarations for uploads, queries, readbacks, presentation, global bindless/BDA resources and opaque callbacks. Unknown usage pins a pass or section conservatively; do not silently reorder.

## Compiler

1. Flatten feature tree; validate IDs, descriptors, imports, pass access types and side effects. Normalize by resource identity (including multiple logical aliases) and subresource range. Reject conflicting same-pass layouts unless explicitly supported. Track whole resource conservatively when range is unknown.
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

Recommendation: first enable compiled reordering on a single graphics queue, retain existing EVSM path as compatibility mode; do not delete async implementation before parity. Then explicitly authored submit sections, with opt-in queue assignment and no reordering across fences. Validate exclusive per-section resource use on a queue or explicit supported concurrent read contract; cross-queue resources use concurrent sharing or explicit ownership protocol, semaphore stages and state handoff. Aliasing across queues forbidden unless happens-before is proven. Full barriers are distinct from semaphore submission dependencies. No automatic queue assignment or split barriers.

## Migration and review checkpoints

A. CPU-only compiler and synthetic tests; no renderer behavior changes. New files under Rendergraph, keep Vulkan lowering separate. Tests for independent chains, all hazards, read group spanning batches, same physical alias, subresources, layout mismatch, side-effect fence, deterministic order, access/stage legality and invalid declarations.
B. Compiled executor, single queue, insertion-order mode first; dynamic import boundary state, per-flight binding, ordinary barriers, graph dump. Port triangle/blur/fuzz samples; compare legacy executor; no aliasing changes yet.
C. Enable grouped reads and ASAP batches behind opt-in. Reuse existing packer with batch lifetimes and explicit alias handoffs; test first use/load, overlapping intervals, multi-flight reuse and resize. Validate with synchronization validation and golden captures.
D. Migrate WorldRenderer feature by feature: ForwardPlus depth/cull, geometry and shadows, postprocessing, UI, then scene/terrain/particles and temporal systems. Replace frame-local captures with stable record functions and typed data. Retain fallback until feature parity; audit every bindless/BDA access and hidden side effect before enabling reorder.
E. Cache variants and remove per-frame graph build on migrated path. Exercise camera motion, feature toggles, swapchain recreate, shader hot reload, capacity growth and history resets. Reintroduce manual queue sections after single-queue correctness. Optional slack optimization and buffer aliasing separate PRs.
F. Benchmark and decide default; remove legacy path only after validation on supported platforms.

## Acceptance

CPU tests deterministic and device-independent; existing test_rendergraph_contract passes. GPU sync validation clean, no device loss on graph fuzz; representative golden captures match within established tolerances. Test two+ frames in flight, resize, feature toggles, dynamic import alias rejection, async fallback and deferred retirement. Report cold compile cost, cache hit rate, per-frame CPU build/record time, batch count/barrier calls, heap peak/aliased bytes and GPU timings on same scenes/settings/hardware against baseline. No promised performance gain without measurements. Cache hit instrumentation must show zero graph builds/sorts/heap packs and no interior hazard analysis; only patch imported boundary states and record. Preserve debug profiling/checkpoints and hot reload.

## Decisions requested from maintainer

1. Does static trees mean one composed graph per renderer/view configuration, or an independently compiled tree of reusable subgraphs that can execute separately? Recommend composed feature tree flattened into one plan, with subgraph declarations.
2. Queue policy: single queue for first compiled path while preserving legacy EVSM, or compile existing async sections from day one? Recommend former.
3. API migration: compatibility adapter around current RenderPass/feature modules, or clean break? Recommend adapter, but new static callback ABI is mandatory.
4. Variants: prewarm at explicit settings/level transitions with bounded cache, or one fixed graph with maximum topology and dynamic empty work? Recommend hybrid: fixed topology for frame variability, variants for structural changes.
5. Initial subresources: conservative whole-resource tracking with precise ranges added incrementally, or full mip/layer/aspect tracking from first release? Recommend exact ranges in compiler model, whole-resource fallback for unknowns.

No runtime implementation or merge is authorized by this RFC; follow-up implementation PRs are review checkpoints.
