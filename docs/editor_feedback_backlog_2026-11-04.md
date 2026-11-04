# Editor feedback backlog — 2026-11-04

> **Scope:** Problem descriptions and requested behavior only. This document is an intake/specification commit, not an implementation. All checkboxes intentionally remain unchecked. The reporter will perform native/visual testing after implementation. Keep related fixes together when planning the implementation PR, and distinguish existing behavior from proposed architecture.

## 1. Docking, render alignment, and input

- [ ] **Scene disappears after docking.** Repro: render an editor scene, then dock/retab/resize the Viewport; the content can vanish even though the Viewport window remains visible. **Expected:** the scene remains visible in the active docked viewport, and switching tabs/undocking/returning restores it.
- [ ] **F1 vs F2 displacement.** The F1 Lit image appears within the teal viewport border, but the F2 Unlit/legacy pass is visibly offset or outside that same border. **Expected:** both modes use the same viewport-space origin, clipping, dimensions, aspect, camera framing and chrome alignment. Check after resizing, moving the primary OS window away from desktop (0,0), docking and switching F1/F2 repeatedly.
- [ ] **Overlapping editor windows steal or lose clicks.** Floating/docked windows overlap the scene; clicks sometimes reach the scene or wrong window. **Expected:** topmost hovered ImGui window/popup owns input; the 3D viewport receives input only inside its exposed client area, with intentional drag capture releasing correctly. No click-through into the scene.
- [ ] **Docked/floating viewport safety.** Maintain correct clipping and restore after splitter resizing, dragging inside/outside the main swapchain and selecting another dock tab. Do not promise scene rendering in secondary platform windows unless a dedicated target exists.

## 2. Inspector hierarchy and presentation

- [ ] **Remove duplicate hierarchy controls.** Delete the Inspector's `Parent: ...` row and `Detach to root` button. Existing reparenting remains available from the Outliner/commands.
- [ ] **Show ancestry as a breadcrumb above the properties.** Example: `Node 2 > Node 104` (full parent chain, updating after reparenting, robust against invalid/cyclic data).
- [ ] **Remove redundant separators.** Inspector sections currently have repeated lines and unnecessary visual dividers; show concise grouping without double separators.
- [ ] **Environment node as a proper property.** Replace the isolated `Mark as Environment node` / `Unmark` action block with the same field presentation used for `Static`, `Bake lightmaps`, etc. Keep `Use as current IBL source` as an available action when relevant.
- [ ] **Shared `Name : Value` layout.** All editable Inspector fields should align in label and value columns, with helpful tooltips. Avoid mixed checkboxes, chips, and ad hoc buttons for equivalent booleans.
- [ ] **Off/On selector for boolean plugs.** Present checkboxes as a two-state enum `Off / On`; preserve existing edit and undo semantics.

## 3. Overrides integrated with properties

- [ ] **Remove the separate Inspector Overrides summary.** Its repeated visibility/material/lightmap/dither rows are noisy. Keep the dedicated Overrides *tool/window* for rule authoring, but put the evaluated status beside each property where it is edited.
- [ ] **Color overridden labels.** Use yellow/pink (as requested; accessible, consistent local-vs-wide styling) to distinguish overrides from ordinary/inherited values. A value such as **Scale X** must be highlighted individually where it is actually overridden, rather than coloring unrelated X/Y/Z values.
- [ ] **Expand override coverage to all compatible properties.** The current hard-coded attribute set (visibility, material, lightmap exclusion/participation, dither) is insufficient: translations, rotations, scale components, scene node flags, lights, materials and component properties should participate as supported by their typed plugs. Local overrides should take precedence over wide rules/inherited values, and undo/revert should restore the correct source.
- [ ] **Typed property/plug registry.** Introduce a stable registry for editor node types and property definitions (e.g. bool, int, float, enum, vector, resource reference). Each plug should have a stable ID/path, display name, type, getter/setter, validation, ownership/serialization metadata, override behavior and optional UI hints. Individual vector axes need addressable plugs.
- [ ] **Maya-style architecture without replacing the scene graph.** Register node types and component types declaratively, use shared property traversal/widgets and an override evaluator, and adapt existing Scene/World/command structures behind this layer. Preserve compatibility with existing `.tour` documents, asset GUIDs, edits and undo/redo; define a migration strategy before changing serialized override formats.

## 4. Material editor and layered materials

- [ ] **Make material flags consistent.** Double-sided, casts shadows, receives contact shadows, etc. should be normal typed `Name : Off/On` properties, not chips with irregular SameLine positioning; alpha mode remains a typed enum.
- [ ] **Layered materials are distinct in the editor.** Normal materials should not expose unused `Layer 1/2/3` and vertex-color-weight fields. Offer a distinct `Layered material` selection/type/presentation; show layer assignments/count and layer material navigation only for layered materials.
- [ ] **Keep render/data compatibility.** Do not silently replace plain materials or lose existing layer references while changing the editor presentation.

## 5. Top-level menus and panel gears

- [ ] **Window menu like a desktop IDE.** Add a top-level `Window` menu alongside File/Create/Edit/Mode, with checked open/closed entries for Viewport, Outliner, Assets, Inspector, Material, World Map, Console, Reference, Overrides and other relevant tools. Provide layout reset, keeping persistent docking state.
- [ ] **Workspace preferences use `Name : Value`.** The Options > Workspace tab currently shows standalone checkboxes. Match the shared editor property layout and Off/On enums.
- [ ] **Shared panel settings gear.** All panel types should have a consistent gear icon and dropdown for panel-specific settings. The common control should be reusable through the panel framework rather than hand-crafted per panel.
- [ ] **Move Non-DAG into Outliner gear.** Remove the visible `non-DAG` checkbox beside the Outliner filter; expose the same setting inside its gear dropdown. Keep the toggle's existing behavior and selection semantics.
- [ ] **DCC actions in the top bar.** Remove DCC link/open/publish/unlink controls from the Assets-browser toolbar; provide them in the top application menu/bar instead. Enable them **only when exactly one appropriate asset is selected**, and disable them for empty or multiple selections.

## 6. Asset browser

- [ ] **Remove redundant Path line.** The upper folder breadcrumb already shows `Folder > Folder > Folder`; remove the separate `Path: ...` display underneath.
- [ ] **Show current item count.** Display `1 item` / `N items` in the browser for the current folder or filter view; count visible assets/folders, excluding hidden metadata, generated artifacts and virtual navigation tiles such as `..`.
- [ ] **Rescan as a two-arrow refresh icon.** Replace the `Rescan` text button with a recognizable two-arrow refresh icon, retaining the existing action and a descriptive tooltip.
- [ ] **Hide generated import artifacts.** Browser navigation should prioritize source/user-authored content, not generated `.geometry`, derived material/animation blobs, import manifests and internal intermediate files. Source-level navigation/drag-and-drop should still resolve usable subassets.
- [ ] **Store generated data in project-local hidden library.** Move imported/generated artifacts into a managed `.artifacts` or `.library` directory (precise name to be chosen), rather than leaving them mingled with sources inside `content/models`. This is a *pipeline migration*: update importer, manifest-relative paths, GUID registry, scene referencing, watcher exclusions, reimport, cook, reload and compatibility with projects already containing in-place artifacts. Avoid moving files without a coordinated resolver/migration.

## 7. Import flow and scene placement

- [ ] **Single Import entry point.** Remove the Assets-browser `Import asset...` toolbar button. `File > Import asset...` should be the canonical entry.
- [ ] **Dedicated Import window.** Selecting File > Import opens a persistent Import window (reopenable from Window), including a source picker, detected import type, relevant format-specific options, destination/behavior, and meaningful progress/results. Show supported formats accurately; do not offer unsupported conversions.
- [ ] **Reimport uses task/cook modal.** `Reimport` currently lacks the existing cook progress modal. Show current reimport activity, messages, success/failure and progress as available; avoid a frozen UI or misleading determinate progress if importer is synchronous.
- [ ] **Add to scene fails when a scene is loaded.** Observed log:
  ```text
  [Editor] References: cannot add asset — no source scene loaded.
  [Editor] Assets: dropping geometry asset 'F:/projects/game/content/models/bistro/bistro.geometry' - placing its content (asset bind).
  [Editor] References: cannot add asset — no source scene loaded.
  ```
  **Expected:** Add to scene and geometry drag-drop work with an open `.tour` scene **even when no glTF source path is active**. Gate on a valid loaded scene/world and writable/referenceable tour target, not solely the glTF path. Check reference insertion, GUID resolution and reload behavior, avoiding lost unsaved edits. The garbled `ΓÇö` dash in logs also suggests a Windows console encoding issue worth checking.

## 8. Task modal and Maya-like bottom status bar

- [ ] **Keep modal Close visible.** Long result/stage text pushes the task popup's `Close` button past the right edge. Reserve button space, wrap/truncate long text, provide full message via tooltip/log.
- [ ] **Current operation in bottom bar.** Show what the editor is doing now (cook, GI bake, import/reimport, DCC publish, etc.), with status/stage and progress widget when available.
- [ ] **Last command executed.** Show the last user command/action and/or most recent meaningful result (Maya-like command/status line) rather than only a generic Ready label.
- [ ] **Responsive layout.** The left message must truncate within its allotted width. Keep progress indicators and task-window reopen controls aligned and visible on the right at narrow window sizes.
- [ ] **Single task status source.** The task modal, status bar and operation logging should agree; backgrounding a task must not erase its progress/result.

## Requested delivery and verification

- [ ] This **initial PR is documentation only**, to record the complete observed problems and desired outcomes before code changes.
- [ ] Subsequent implementation should address the whole scope, with coherent shared abstractions instead of local style patches.
- [ ] The reporter will run native builds, tests and visual checks; document changes truthfully and do not mark unverified items as complete.
- [ ] Confirm compatibility paths for `.tour` serialization, project content databases, import artifacts and docking before shipping.

## Reproduction/acceptance walkthrough

1. Open a scene and dock/resize/float Viewport; compare F1 and F2 against the teal border, including after moving the main OS window.
2. Overlap panels and dialogs with the Viewport, then click UI fields and scene geometry; confirm the intended topmost target receives the input.
3. Select and reparent nodes; inspect breadcrumb, separators, boolean fields, environment flags and overridden individual transform axes.
4. Inspect normal and layered materials; confirm flags use consistent typed fields and layer slots appear only for the latter.
5. Open Window menu, toggle panels, use a common gear (including Outliner Non-DAG), resize/resume dock layouts.
6. Browse folder/filter views, verify breadcrumb and item counts, click refresh icon, check hidden generated files.
7. Import/reimport a model, verify task progress, and add derived geometry to an already loaded `.tour` scene.
8. Run a long cook and force a long failure message; verify task popup Close remains visible and bottom bar shows operation, progress and last command at several window widths.

**Source:** Reporter screenshots and logs from the editor discussion on 2026-11-04.
