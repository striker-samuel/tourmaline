# Terrain and GPU-instancer brush presets

Brush presets are small, inspectable `.brushpreset` text assets in
`editor/brush_presets`. Create one with **Save brush preset** in the
terrain inspector or Placement Graph, or edit the file directly.
The editor discovers these assets on launch or when opening the combo.
When launched from a build directory, the editor searches up for its
source checkout. Custom presets written there can be checked into version
control with the rest of the project.

## Terrain sculpting

Select a terrain group or tile; in the inspector enable **Paint mode**.
Pick a terrain preset, or adjust **Brush** (Raise/Lower/Flatten/Smooth),
**Radius**, **Strength**, and **Stroke spacing**. Hold LMB in a 3D viewport
to stamp along the terrain. Spacing is a *fraction of the radius* (0.2
gives closer stamps than 0.5). The world-space outline indicates the
brush footprint. Each stamp uses the existing undoable `terrainPaint`
command and the existing deterministic terrain replay.

## GPU-instancer density painting

Open **Placement Graph**. Select a scatter preset (which updates the
graph's Density, Transform and Spacing parameters) and enable
**Paint GPU instancer density**. The first activation inserts an
`EcotopeMask` multiplied with the graph density, before the Accept
node (or the Layer input when Accept is absent). This is a real IR field,
not a post-filter of the editor's dots; the engine has both CPU and GPU
evaluators for this operation. Painting only creates an authoring mask
for this active graph.

LMB paints coverage; **Erase** removes it. **Radius**, **Opacity**,
and **Spacing** control the stamp. **Undo scatter stamp** removes the
last stamp; **Clear scatter mask** empties this graph's mask. Scatter
stamps persist as `place.paint.<graph>.<index>` editor settings in
the `.tour`, separate from mesh assets and from the graph topology.
The brush mask is rebuilt deterministically on preview refresh, with
256x256 samples per placement domain and bilinear sampling by the
ecotope evaluator. The mask starts at zero (no instances) until painted.

**Important:** This PR wires the editor's preview to the shared mask
evaluation path, but does not add a separate GPU upload / runtime streaming
asset pipeline for brush masks. An integration using GPU runtime placement
still needs to bind the generated `EcotopeMaskData` and verify the GPU
result against CPU. Brush-stroke undo uses the dedicated button, not the
Placement Graph topology Undo button.

## Asset format

```ini
version=1
kind=terrain
name=Soft Sculpt
radius=12
strength=0.35
spacing=0.2
operation=0
density=0.5
minScale=0.8
maxScale=1.2
minDistance=0
```

`kind` is `terrain` or `scatter`. Fields are validated before a
preset applies. For terrain `operation` is 0 Raise, 1 Lower, 2 Flatten,
3 Smooth. For scatter, `density` controls the graph Density node,
`minScale/maxScale` control the Transform node and `minDistance`
controls the Spacing node. Other fields describe the brush shape.
There is one editable file per named preset.

## Manual verification

1. Create a terrain, load Soft Sculpt, paint a stroke, check spacing,
   flatten/smooth, Undo/Redo and save/reopen.
2. Open a Placement Graph, load Dense Foliage, enable scatter paint and
   paint/erase over a grid or bound terrain; verify that the point
   count responds to density coverage.
3. Verify a second graph's stamps are independent, then save and reopen
   the `.tour`.
4. Test perspective and orthographic viewports, large meshes, empty
   sky and rapid mouse strokes. Profile the 256x256 mask rebuild at
   high stamp counts.
5. Confirm GPU placement upload and evaluation parity in the runtime
   integration. The editor's placement preview is CPU-generated.
