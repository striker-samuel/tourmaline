# Editor scene, asset and prefab workflow

## Create a scene

With a project open, use **File > New scene...** to create an empty,
authorable `.tour` file in `content/scenes/`, or navigate to a content
folder and choose **Assets > New... > New Scene (.tour)** (also available
from the asset-grid background context menu). New scenes use unique
filenames: `New Scene.tour`, `New Scene 2.tour`, etc. They are registered
with the project asset database and opened in the editor when it is safe
to switch. If the current scene is dirty, the new asset is created and
selected but the current scene stays open until you save/switch explicitly.

Use **New Folder** from the same menu. Both actions are contained inside
the project's `content/` tree and refuse to follow symlink escapes.

## Reimport assets

Select a **source** model (`.gltf`, `.glb`, `.obj`) or a
`.texture` asset in Assets and click **Reimport**, or right-click its
tile and choose **Reimport**.

- Model reimport forces rederivation, even when the automatic staleness
  gate considers the artifact current. The existing identity ledger is
  reused; a bound live scene is reloaded through a temporary `.tour`
  snapshot to keep its unsaved document changes.
- Texture reimport uses the source image specified by its `.texture`
  definition and the existing force-refresh path for live texture
  bindings. If no scene/texture is loaded, the next scene load imports
  it normally.
- Derived geometry/material/animation output is deliberately **not**
  reimported directly. Reimport its owning source model. A `.tour`
  document is opened or reloaded rather than imported as a mesh.

Automatic file-watcher refresh and **Rescan** remain independent:
Rescan reconciles the asset directory / metadata, not GPU resources.

## Prefab editing

The existing **Prefab World** pane remains available for simultaneous
source/parent preview. Its node list now renders the hierarchy with
expansion and a filter instead of a flat list. Its limited transform
authoring remains unchanged.

Choose **Edit in main editor** in the prefab pane, or right-click a
`.tour` asset in the asset browser and choose **Edit in main editor**,
to enter full-scene prefab mode. This makes the prefab the current
`.tour` in the normal editor, giving it the same outliner, inspector,
gizmos, undo/redo and creation tools as any other scene.

The switch is performed at the frame boundary, after writing a temporary
snapshot of the original scene's working document. The original scene
file is not saved or overwritten. Use:

- **File > Save prefab & return to scene** to persist the prefab and
  reopen the original scene from its temporary working snapshot
- **File > Discard prefab edits & return** to reopen the original
  scene without saving the current prefab document

On return, the original save target, dirty flag, orbit cameras and
selected nodes are restored where possible. The temporary snapshot is
deleted only after a successful return. Uncommitted changes from the
separate prefab pane must be saved or reverted before entering full mode.

## Known limits / additional sensible follow-ups

This is a focused workflow patch, **not** a complete replacement for
the editor's asset pipeline or a generalized multi-document session
manager. The separate prefab pane still supports TRS-only editing
(renaming, reparenting, materials, gizmos and other structural edits
require full-scene mode). There is only one full-mode return slot.
The parent snapshot restores authored scene changes and common view
state; it does not serialize every transient UI/panel/tool state.
Asset renaming, duplication, bulk reimport, a reimport progress modal,
and fully incremental scene-instance rebinds remain future work.

## Verification

- Headless creation-helper cases are part of `AssetOpsTests`;
  run `ctest --output-on-failure -R asset_ops_tests`.
- Build the full `Editor` target and interactively verify:
  create an empty scene; add a locator/light; save/reopen; reimport a
  referenced model with unsaved tour edits; inspect a refreshed
  `.texture`; enter a prefab in full mode, save and return; repeat
  with discard; verify the parent document's dirty state and selected
  node survive; verify the prefab pane's expanded hierarchy/filter.
- GPU/ImGui interactive behavior requires an Editor build and manual
  viewport smoke test. The headless test does not certify it.
