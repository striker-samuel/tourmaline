# State Machine Graph Editor

The editor's **View → State machine graph...** window is a visual
authoring tool for the existing versioned `.statemachine` format. It does
not create a second engine graph runtime, a new asset kind, or a custom
scripting system. It is built on PR #57's
`tour::graph::Document` / `StateMachineTopology`.

## Open / create

Select a `.statemachine` file in the **Assets** panel and click
**Edit state machine graph...** in its Inspector, or open the graph
window from **View**, then press **New**. For a new graph enter a
`.statemachine` destination in the path box; save writes the
corresponding source file.

Unsaved changes block opening another file or reloading; the window
provides explicit **Discard & new** and **Discard & reload** actions.
The file is strictly parsed on open (1 MiB limit), validated against the
runtime parser before save, and written through a sibling temporary
file followed by atomic replacement. The original stays intact if
validation or writing fails.

## Canvas / inspection

- Click a node to select it; drag to reposition.
- **Middle mouse** pans; wheel zooms around the cursor.
- Click an output pin, then a compatible input pin to create a
  transition. A parameter is required first; new transitions default
  to the first parameter.
- Click a wire to inspect its condition, choose the parameter,
  comparison, and threshold. **Priority up/down** changes evaluation
  order (first successful transition wins). Delete removes the wire.
- Add, rename, delete and select the entry state; the **Any**
  node represents wildcard-source transitions.
- Create typed **bool/int/float/trigger** parameters, edit their
  defaults and remove them (referencing transitions are removed).
- Toolbar Undo/Redo handles structural/parameter edits. Layout dragging
  is saved but does not yet create undo steps.
- Graph-specific Delete/Backspace takes precedence over scene-object
  deletion while this window is focused.

The canvas renderer (`editor/src/editor_graph_canvas.h`) depends
only on `tour::graph::Document` and ImGui. PlacementGraph keeps
its existing UI today; it or future ecotope/behavior editors can
adopt this canvas independently of the state-machine inspector.

## Storage compatibility

Graph syntax remains the source of truth:

```text
statemachine 1
param speed float 0
param jump trigger
state idle
state moving
entry idle
transition idle moving speed > 0.1
transition * idle jump fired
# editor-pos idle 24 40
# editor-pos moving 290 100
```

The graph editor reserializes source declarations in their canonical
order and appends `# editor-pos <state> x y` comments. The existing
runtime/cook parser strips comments, so no additional `.graph` or
position-sidecar asset is required.

**Note:** saving a file through this first editor version canonicalizes
the text, so arbitrary hand-written comments and formatting are not
preserved. The editor-specific positions are preserved.

## Dependency and tests

This change is stacked onto PR #57 because it requires the
`state_machine.h` asset and the shared graph model. Review it against
`feature/state-machines-navigation-assets`, not a `master` that
doesn't yet include those definitions.

`GameplayAssetTests` now covers serializer round-tripping through
the actual runtime parser, typed parameters, transition priorities,
runtime evaluation of saved graphs, and the editor layout comments.
Full Editor build + manual canvas/window/asset-browser smoke testing
are still needed prior to marking ready for merge.
