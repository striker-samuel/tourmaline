# Editor dockable workspace (ImGui docking)

The editor opts into the already-pinned Dear ImGui **docking branch**.
The application/game ImGui contexts are unchanged.

## Working with the layout

- Drag a tool's **tab/title bar** to dock it to an edge, split a region,
  tab it next to another tool, or float it *inside the editor window*.
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
Dear ImGui supports cross-OS-window platform viewports with
`ImGuiConfigFlags_ViewportsEnable`, but Tourmaline does *not* enable them:
its renderer records to one swapchain and has no secondary-window
render/present path. Turning on the flag alone cannot make panels render
outside the main OS window. Floating within that window remains supported.

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
9. Lighting > GI volumes > Lattice (pre-bake): run with and without the
   separate lattice preview fix PR; the dockspace must not obstruct the
   editor-drawn probe spheres when its pass is correctly ordered.

A native build and visual capture are needed to sign this off; static
code review is not equivalent to testing Dear ImGui docking plus Vulkan.
