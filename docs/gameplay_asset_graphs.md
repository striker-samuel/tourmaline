# Shared graph foundation, state machines, and navigation

This change adds two first-class gameplay-data assets and one **engine-level, domain-neutral graph topology layer**. It does not introduce a prefab, UI layout, input-map, or hard-coded AI architecture.

## Why a general graph core?

Placement already has a Maya-style authoring dependency graph in `editor/src/editor_dg.h` and a separate placement instruction IR in `Engine/Placement/placement_ir.h`. Ecotope masking is already a placement node. A replacement graph VM would duplicate and disrupt those systems.

`Engine/Graph/graph.h` instead offers a pure C++20 `tour::graph::Document` with stable numeric node/link IDs, a domain-chosen node type string, named typed ports, input multiplicity rules, and a deterministic topology validator. The validator catches malformed links, incorrect port types, multiple input wires and cycles, with an explicit `allowCycles` switch for state machines/feedback systems.

The **domain owns evaluation**, node schema lookup, parameter values, rendering, persistence and graph compilation. Placement's existing `CompileGraph` now adapts its node/pin schema into this shared structure before generating its established placement IR. A future ecotope editor, behavior graph, audio graph, shader graph, or another placement type can reuse this substrate without adopting placement operations.

There is deliberately no new generic `.graph` asset yet: a shared graph *data model* does not mean every graph must use the same authored syntax or runtime semantics.

## State machines: `.statemachine`

A small, strict, versioned textual format:

```text
statemachine 1
param speed float 0
param dead bool false
param jump trigger
state idle
state run
state falling
entry idle
transition idle run speed > 0.1
transition run idle speed <= 0.1
transition * falling jump fired
```

- Parameters: `bool`, `int`, `float`, `trigger` (one-tick).
- Forward references are allowed. Transition evaluation is authored-order deterministic, **first true transition wins**, maximum **one transition per `Tick`**.
- Trigger parameters expire at the end of the tick (even if a higher-priority transition won).
- `StateMachineAsset` is immutable shared data; each `StateMachineInstance` holds independent state and typed parameter values.
- `Tick()` returns an optional `StateChange`; it does **not** execute callbacks, animation clips, input bindings or game code.
- No time-based transitions or nested states in this first slice. Applications own their update frequency and any higher-level policy.

```cpp
auto bytes = runtime.ReadAsset(project::statemachines::movement, err);
tour::asset::StateMachineAsset doc;
if (bytes && tour::asset::ParseStateMachineText(
    std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
    doc, err)) {
    tour::asset::StateMachineInstance machine(doc);
    (void)machine.SetFloat("speed", 2.0);
    if (auto change = machine.Tick()) {
        // Game code decides how to respond to the state transition.
    }
}
```

The parsed document must outlive its borrowed instances. A future app-layer cache can enforce this automatically.

## Navigation: `.navmesh`

A precomputed triangle-surface format (this is **not** a scene-geometry-to-navmesh baker):

```text
navmesh 1
vertex 0 0 0
vertex 10 0 0
vertex 10 0 10
vertex 0 0 10
triangle 0 1 2
triangle 0 2 3 1.5
```

- World coordinates: XZ traversal plane, Y height; triangles must be non-degenerate in XZ.
- Edge-sharing adjacency is built at parse time, with non-manifold edges rejected.
- `LocateNavTriangle` picks the closest height plane for overlapping floors; off-surface queries return no location.
- `FindNavPath` runs deterministic shortest-cost search over triangle adjacency and emits a **raw corridor** (centroids + endpoints), without string pulling/funneling.
- This slice has no agent radius/clearance, dynamic obstacles, avoidance, off-mesh links, automatic bake, multi-cell joins or streaming provider yet. It is useful for validating the asset and content lifecycle before adding those more sophisticated runtime pieces.
- Future `.navbuild` authored cook settings should produce the `.navmesh` artifact from collision geometry, registered through the existing `WorldPartition` CPU-provider seam. The navmesh itself is not a collision source.

## Content pipeline

- Recipe includes accept `*.statemachine` and `*.navmesh`.
- Asset database allocates append-only 0x9xxx and 0xAxxx identity buckets; generated `project.h` emits `project::statemachines` and `project::navmeshes` typed keys and query traits.
- New catalog kinds append **8** and **9**; new pak payload kinds append **18** and **19**. Existing sprite-set kinds 7/17 remain unchanged.
- The project's ids pass and pak content-set cooker both validate content strictly before packing. Paks store validated authored bytes verbatim; `ContentRegistry` and `ProjectRuntime` resolve them via stable IDs and VFS paths, preserving override behavior.
- CPU regressions in `GameplayAssetTests`: parser failures, state transitions, graph validation and navmesh path queries.

## Project boundaries

- Reusable scenes/prefabs remain an extension of `.tour`.
- UI layout and input mapping stay application-defined, using the engine's lower-level UI and input facilities.
- Graph models define **structure**; game logic defines **meaning**. No generic scripting VM or forced component system.
