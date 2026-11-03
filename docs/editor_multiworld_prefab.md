# Multi-world prefab authoring — live .tour source editing

This feature adds a **second editable \`tour::World\`** to the existing editor without adding a \`.prefab\` asset kind, a new serializer, a prefab ECS, or a second RHI device. The first-cut scope is live transform authoring on the **source definition**, with correct parent-instance precedence and independent undo/revert.

## Open a prefab

Open a parent \`.tour\` scene, then choose any of these routes:

- **Reference editor** → a \`.tour\` row → **Edit prefab**
- **Outliner** → select a reference placeholder → Inspector → **Open prefab world**
- **Assets** → select a \`.tour\` source → **Edit as prefab world...**

The second window initially docks **beside** the parent world's central stage (when there is enough room and the editor is in the single viewport layout). Use **Float pane** to detach it; **Dock pane** to return. The parent and prefab viewports are independent and both remain visible. Small windows and quad view use a floating prefab window.

The prefab window has its **own** scene hierarchy, selected node, orbit camera, editable local TRS, undo/redo, Save and Revert controls. The parent editor's outliner, selection, scene undo stack, camera and source document are untouched.

## Ownership and renderer lifecycle

\`\`\`text
EditorApp
├── shared RHI device, asset-db/cache, and editor rendergraph
├── parent World + parent .tour document/history + existing WorldRenderer
└── PrefabWorldSession
    ├── separate World (own Cluster, scene, geometry, materials)
    ├── separate .tour::Doc + saved baseline + undo/redo
    ├── selection + orbit camera + root/child local transforms
    └── second WorldRenderer (borrows that World)
        └── inline target → scoped opaque composite → prefab child viewport
\`\`\`

Both renderers run in the existing **WorldLayer** boundary/record rhythm; loading and destruction happen **before** the graphics acquire, never halfway through an ImGui draw or Vulkan pass. Changes to inline-target extent use the same 16-pixel resize deadband / after-present quiesce guard as the primary live renderer.

The secondary World builds its own Cluster from the authored source with the existing \`ApplySceneReferences\` and \`ApplyTourToCluster\` flow, rather than reusing the parent's flattened node array. The source uses **normal .tour semantics**. The second renderer is a separate instance of the engine WorldRenderer with \`SetBorrowedWorld\`; the editor retains world ownership.

## Live synchronization (option A)

When a local TRS changes in the prefab world:

1. The source World updates **immediately**, without saving the .tour.
2. Each parent **reference occurrence** to the same canonical .tour source is discovered.
3. The corresponding source node is resolved by the existing reference namespace and node name, **inside that occurrence's subtree**.
4. The change is applied only if the current instance-local transform still equals the saved source transform and the parent has no local TRS override.
5. The parent scene marks the node dirty and recalculates global transforms; the parent WorldRenderer sees the update on its next tick.
6. Both parents and prefab rendering happen with distinct World and camera data; neither changes the other's selection or history.

The in-memory preview is explicitly **not** a parent \`.tour\` command, so saving the parent scene cannot accidentally commit unsaved source edits. Unsaved prefab changes are undone by **Undo** or **Revert unsaved**. Closing a dirty prefab window requires Save or Revert first; the editor's full application shutdown autosaves a dirty prefab source, matching its existing parent-scene shutdown policy.

For safety, each touched parent node records an original pose and the exact last preview pose. If the parent edits that node independently, the preview stops claiming it and never rolls it back later. If there are explicit instance TRS overrides, those win. The headless policy lives in \`editor/src/editor_prefab_sync.h\` and has CPU regression coverage.

**Definition Save** writes the *prefab source* \`.tour\` only, via a same-directory staging file and atomic replacement. On success the displayed pose is the new saved baseline. A subsequent Revert restores that saved state; Save does not write a parent instance override.

## UI and authoring

- In the prefab pane, select a source node from its own hierarchy.
- Edit **Translate**, **Rotate (XYZ degrees)** or **Scale** with ImGui drag controls. Edits synchronize live during dragging, not only when Save is clicked.
- On drag completion, append the corresponding existing \`smvk::tour::Verb::SetTranslate\`, \`SetRotate\`, or \`SetScale\` command in the prefab's source document (cluster-space index and stable name).
- Undo/Redo capture isolated source document + source local transform snapshots, then reapply or revert live changes across matching parent occurrences.
- Revert restores the saved source and every previewed parent node still at the pose this editor had written. Closing a clean prefab releases its GPU World. A successful Save preserves the new source pose in all affected parent occurrences.

## Deliberate limitations (draft PR)

This is the **first vertical slice** of full multi-world editing, not a complete migration of the monolithic editor session:

- Only **local TRS** edits are available in the prefab pane today. Renaming, hierarchy restructuring, deletion, geometry/material authoring, and custom per-node attributes need more extracted session-aware editing services; they are **not** silently reflected across worlds.
- Exactly **one secondary prefab world** can be open alongside the main world. A general N-world tab manager can follow once editor-wide selection/history/gizmo ownership is extracted fully from \`EditorApp\`.
- The first sync mapping is **namespace + node-name** based. If source nodes are ambiguous or renamed during a session, structural UUID-based remapping and subtree rebuilds are future work; source rename editing is intentionally disabled.
- Unchanged source nodes are not pushed over local overrides. Full general-purpose merged-override semantics need a dedicated per-property rebase/inheritance model. Transform overrides are explicitly respected by this slice.
- The second renderer uses separately loaded cluster resources, not an optimized shared instance template/cache. GPU memory and frame cost scale with a second WorldRenderer.
- Parent-world transform updates are lightweight; **structural changes** would require full reference subtree rebinding and source-child ID recovery. No structural mutations are exposed in this initial UI.
- Prefab selection is via its outliner, not GPU picking within the second viewport. Orbit/zoom work independently. Material editors, create/duplicate, normal gizmo tools, scene Bake/Cook, and the general scene command stack remain parent-scoped.
- The prefab composite targets only the child viewport rect. It draws after the ImGui pass because the editor window paints a background; overlapping popups *over that rectangle* are not yet fully solved by an ImGui-native texture bridge.
- The actual Editor/Vulkan build and interactive two-renderer lifecycle need CI and a manual smoke test; the GPU code is not certified by the headless tests.

## Verification

Build Editor with \`editor_prefab_multiworld.cpp\` (the editor CMake source glob includes it). Run:

\`\`\`sh
ctest --output-on-failure -R 'prefab_live_sync_tests|tour_tests|pak_host_tests'
\`\`\`

Then open a parent \`.tour\` with two references to the same child \`.tour\`, open **Edit prefab**, change source node translation, verify both unoverridden instances move, undo/revert and verify both return, override one instance in the parent and verify it no longer follows source transform changes, save prefab and reload both source and parent, close pane and reopen, and repeat after resizing/undocking.

The long-term goal is to extract editor session state (selection, history, world, camera, tools, node targeting, asset edit permissions) into a general \`EditorWorldSession\` manager supporting multiple simultaneously editable \`.tour\` documents. This PR provides an **independent second World and authoring document today**, with an intentionally narrow set of safe live-edit operations.
