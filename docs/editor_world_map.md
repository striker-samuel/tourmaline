# World Map (independent grid with terrain overlay)

The **World Map** is a dockable, CPU-rendered orthographic top view of
world X/Z. Its world grid is drawn even when no terrain group exists. It does not move or replace the primary
Vulkan scene viewport. Find it beside Viewport in a reset workspace, or
reopen it via **View > Panels > World Map**.

## Controls

- **Fit** centers the selected terrain group, or the world grid at the origin
  when no terrain exists. The grid uses the viewport debug grid's cell size,
  half-extent, five-cell major lines, and emphasized axes. It always appears
  regardless of the viewport Ground grid toggle.
- Select a terrain group in the combo (or select one of its nodes in the
  outliner to follow it) when terrain is present.
- Mouse wheel zooms around the cursor. Middle- or right-drag pans.
- The optional grey shading visualizes sampled global heights; gold lines are
  per-tile boundaries derived from shared grid samples, including uneven
  sample splits.
- When terrain exists, toggle **Sculpt** to stamp Raise / Lower / Flatten / Smooth with left-drag.
  Radius and strength are shared with the existing 3D terrain brush.
  Every stamp uses the regular `TerrainPaint` EditCmd, preserving
  deterministic .tour persistence and undo/redo.
- The map uses X right, +Z down, and displays absolute world X/Z/Y at
  the cursor. Drawing uses a bounded coarse sample grid (at most 96x96)
  and does not allocate extra GPU terrain resources.

## Scope and limitations

The independent world grid needs no terrain resources. Sculpting edits the
editor's procedural **TerrainGroupData** only.
The separate streamed-world H/S atlas and WorldPartition providers are
not yet editable here. This is a top-down heightmap overview rather
than an independent Vulkan scene render target: it can be docked or
floated as an ImGui tool window, including with native multi-viewports
when that system is available. Material splat painting, biome masks,
tile selection, and cross-world streaming are follow-ups.

## Manual validation

1. Open World Map in a scene without terrain; confirm the grid is visible,
   zooms and pans, and Fit centers the origin. Check both grid settings and
   the viewport Ground grid toggle do not disable the map grid.
2. Create terrain with uneven grid/tile divisions; confirm tile outlines
   meet at shared vertices, and zoom/pan remain stable.
3. Sculpt near an edge between two tiles; confirm both tile meshes update,
   then undo and redo and reload the .tour file.
4. Select two different terrain groups and verify that Fit and outliner
   follow work. Hide and reopen World Map, then reset the dock layout.
5. Test pane resizing, native detached windows if enabled, and regular
   3D viewport input routing. Verify no sculpting occurs with Sculpt off.

Source review only; no native editor build or GPU smoke testing was
available during authoring.
