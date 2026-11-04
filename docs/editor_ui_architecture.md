# Extending the Tourmaline Editor UI

The editor is C++/Dear ImGui. The main UI entry point is
`editor/src/editor_ui.cpp`. Feature-specific implementations live in
`editor/src/ui/panels/`, individual inspector implementations in
`editor/src/ui/inspector/`, and dialogs in `editor/src/ui/dialogs/`.
The existing engine, scene and command implementations are unchanged.

## Architecture

- `EditorApp` owns application lifecycle, the renderer, scene and existing
  editable state. During migration, feature .cpp files still implement
  `EditorApp::Draw*()` methods, which is intentional for compatibility.
- `PanelRegistry` owns window Begin/End and panel visibility. New dockable
  windows register a title, visibility bool and callback in `DrawUI`.
- `EditorContext` is a deliberately small view for callbacks. Grow it with
  explicit, narrow services rather than a raw `EditorApp&`.
- `EditorActions` wraps the existing `Do`, `TrackDrag`, and `FinishDrag`
  entry points. It is NOT a new history implementation.
- Property widgets build on `editor_attr_ui.h` and retain its scoped IDs,
  label sizing and tooltip behavior.

## Add a dockable panel

1. Create `editor/src/ui/panels/my_panel.cpp` for the panel contents.
2. During the incremental migration, implement a declared
   `EditorApp::DrawMyPanel()` member and add its visibility state.
3. Register it in `EditorApp::DrawUI()`:

   ```cpp
   panels.Register("My Panel", m_showMyPanel,
       [this](EditorContext&) { DrawMyPanel(); });
   ```

4. Add the panel to View menu and optional default docking layout.
5. Keep the window title stable: ImGui saves dock identities in
   `editor.imgui.ini`. Do not call `ImGui::Begin/End` inside registered
   panel bodies. Existing floating windows with custom flags are separate.

## Add an inspector section

1. Implement `DrawYourComponentSection(uint32_t node)` in a dedicated
   `ui/inspector/your_component_section.cpp`.
2. Declare it in `editor_app.h` and register it with
   `InspectorSectionRegistry` in the node branch of `DrawInspector()`.
3. Reuse `editor::attr` controls and stable `attr::ScopedID` keys.
4. Convert changes to `EditCmd` and execute through the command pipeline.
   Preserve drag coalescing; don't mutate the scene directly in a widget.
5. Verify undo/redo, multi-selection, save/reload and keyboard focus.

## Why a staged migration?

The present editor has a substantial `EditorApp` state surface. Moving all
fields into new owners while simultaneously splitting UI functions changes
ownership/lifetimes and risks regressions in the Vulkan live viewport,
asset browser and undo stack. This extraction first creates independent
compilation units and a panel/context seam, then individual features can
adopt narrower state models without a second massive rewrite.

## Rules

- Draw methods render; system services load/save/edit.
- Preserve existing dock window titles, shortcuts and ImGui IDs.
- The viewport renders through the primary Vulkan swapchain; do not swap
  it for an ImGui offscreen texture as a side effect of UI refactoring.
- Keep `Do/Undo/Redo` and `TrackDrag/FinishDrag` behavior intact.
- `editor/CMakeLists.txt` uses `GLOB_RECURSE CONFIGURE_DEPENDS` for
  `src/*.cpp`, so added feature implementation files are auto-included.

## Validation

Run `python3 tests/test_editor_ui_structure.py` for fast structural checks.
This does not replace a C++ build or interactive Vulkan tests.


Configure/build `Editor` on a supported environment, run existing editor
regressions, and manually check dock restoration, selection, inspector
editing/undo, asset drag-and-drop, material preview, modals, and the
live-view hover/capture lifecycle. See `tests/editor_imgui_id_regressions.md`.
