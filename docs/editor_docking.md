# Editor dockable workspace (ImGui docking)

The editor opts into the already-pinned Dear ImGui **docking branch**.
The application/game ImGui contexts are unchanged.

## Working with the layout

- Drag a tool's **tab/title bar** to dock it to an edge, split a region,
  tab it next to another tool, or detach it as an SDL3/Vulkan native OS
  window (including onto a second monitor).
- **Viewport**, **Outliner**, **Assets**, **Console**, **Overrides**,
  **Inspector**, **Render**, and **Material** are independent windows.
  Optional windows (GI probe viz, Cook & bake, References, Placement
  graph, etc.) use ImGui windows too and can join the dockspace.
- Undocked tool windows have a close button. Use **View > Panels** to
  reopen a closed panel (including the 3D Viewport).
- **View > Reset dock layout** rebuilds the default three-column split.
  It does not touch the current scene, tools or project.
- **I** hides the workspace for a fullscreen viewport; **I** again
  restores the same dock assignments.
- Positions, splits and selected dock tabs are saved by ImGui to
  `editor.imgui.ini`, adjacent to `editor.prefs`. This is editor
  workspace state, **not** `.tour` content. Deleting the ini restores
  the default layout on the next launch.

## Render/input contract

The 3D scene still renders through the editor's existing Vulkan render
graph; it uses the dockable Viewport window's **client area** (not the
whole workspace). It retains the native 4:3 stage and optional quad
layout. ImGui draws the dock/tab chrome over that stage. Mouse movement,
selection, gizmos and camera gestures inside the viewport client remain
scene-owned; popup/panel/tab interactions remain ImGui-owned.

The editor records Vulkan passes **before** building this frame's ImGui
workspace. Resizing or moving a dock therefore has a **one-frame**
viewport-geometry delay. Floating views are clipped to the swapchain
before render-pass offsets are converted to unsigned pixels. An inactive
Viewport dock tab parks the heavyweight live WYSIWYG renderer.

The top tool strip, main menu and bottom status strip deliberately
remain fixed. Docking means rearranging **tool windows**; individual
fields/buttons inside a tool are not draggable by themselves.
The editor enables `ImGuiConfigFlags_ViewportsEnable` before initializing
its SDL3/Vulkan ImGui backends, then uses the backends' built-in secondary
swapchains via `UpdatePlatformWindows` / `RenderPlatformWindowsDefault`
after presenting the primary swapchain. Standalone games do not opt in.

**3D scene limitation:** The scene Viewport still composites directly into
the main window's Vulkan swapchain. It is anchored to the main ImGui
viewport; detachable tools (Inspector, Assets, Console, etc.) are regular
ImGui draw data and render in their own OS windows. Rendering the 3D
scene in a detached native window requires a separate scene render target.

## Visual smoke matrix (requires Editor + Vulkan)

1. First launch with no `editor.imgui.ini`: default Outliner/Assets
   left, Viewport center, Inspector/Render/Material right.
2. Drag Console beside Inspector, float Assets, tab Material with
   Viewport; verify tabs can be moved back and the last active tab stays.
3. Resize a splitter and float Viewport partly offscreen. Viewport must
   render without Vulkan invalid offsets, cropping or input capture
   in other panels.
4. In the Viewport, select an object, orbit, zoom, use the gizmo, change
   the active viewport in quad mode. Popups over the scene must still
   receive mouse input.
5. Toggle **I** off/on: fullscreen scene then the previous dock layout.
6. Exit/relaunch: all dock assignments, sizes and tab order should be
   restored from `editor.imgui.ini`.
7. Float Outliner and Console, close both via their title-bar X buttons,
   then reopen them using **View > Panels**. Close/reopen Viewport and
   confirm the scene draws again, without stale mouse capture.
8. **View > Reset dock layout**: the core windows return to their
   default dock positions, with the current project and selection intact.
9. Drag Inspector or Console outside the main application and onto a
   second monitor. Verify content, mouse/keyboard input, native close,
   and returning the window to a dock. Close a detached window without
   shutting down the editor. Move the main OS window away from (0,0)
   and verify the dock host/status remain positioned correctly.
   **Regression check:** keep a live Lit scene in the docked Viewport,
   move the main native window to a nonzero desktop position (and a
   second monitor when available), then resize the Viewport splitter.
   The live scene should remain inside its client rect rather than
   disappearing, shifting, or clipping at the original desktop origin.
   Repeat with the Viewport floated inside the main swapchain; do not
   expect scene rendering in a detached secondary native window.
10. Lighting > GI volumes > Lattice (pre-bake): run with and without the
   separate lattice preview fix PR; the dockspace must not obstruct the
   editor-drawn probe spheres when its pass is correctly ordered.

A native build and visual capture are needed to sign this off; static
code review is not equivalent to testing Dear ImGui docking plus Vulkan.
