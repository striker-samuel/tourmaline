# Surface Brush (editor)

The **Create > Surface brush (points / spline)** tool paints on the
frontmost visible mesh in any 3D viewport. The brush is intentionally a
lightweight editor tool, not a new cooked asset or procedural graph node.

## Workflow

1. Open a source scene, then choose **Create > Surface brush (points / spline)**.
2. Pick **Points** (scatter guide) or **Spline** (connected stroke), and set
   the on-screen spacing in pixels. Hold LMB and drag across visible surfaces.
   Alt+mouse camera navigation still works; Esc exits the tool.
3. Releasing LMB commits the stroke to the source `.tour`. Moving across
   different objects during a drag starts a new segment, each bound to its
   own surface object.
4. **Undo stroke** / **Redo stroke** operate on complete brush strokes.
   **Bake points as locators** converts the painted samples into real
   undoable scene transform nodes under the matching painted objects.
   (The guide strokes remain available after baking.)

## Data / transforms

`surface.stroke.<index>.meta` stores a version, guide type and the
**parent node UUID**. `surface.stroke.<index>.point.<index>` stores an
XYZ point in that parent's local space. Stroke settings ride the existing
`.tour` editorSettings lane; they are deliberately **not cooked**.
The visible guide transforms through the parent scene global matrix every
frame, so moving or rotating its object also moves the guides. Missing
UUIDs are shown as missing guides rather than silently reattaching to a
different mesh.

Picking uses the existing CPU frontmost triangle raycaster, including the
editor's reversed-Z view-projection. This means no synchronous GPU depth
readback stalls, and supports objects on occlusion boundaries, but the
cost of a dab still depends on the CPU mesh picker.

## Known limits

- No authored curve asset, spline-to-mesh extrusion, or runtime path
  component yet. The spline mode draws connected guide points.
- Painted points follow the **object transform**; they do not attach to
  deforming mesh vertices or survive arbitrary changes to its geometry.
- Each guide holds up to 2,048 points, and the file keeps up to 512
  strokes. Points are placed only on a surface hit (no working-plane
  fallback). Unresolved parent identities are ignored on preview/bake.
- Bake currently creates one locator per sample, with ordinary scene
  undo; very large bakes may be expensive. No procedural instance
  prototype picker is wired into this tool yet.

## Tests

`placement_editor_tests` covers parent-local math, transform-follow,
singular matrices, duplicate hits, capacity, and serialization validation.
Full viewport behavior requires a GPU editor smoke test.

## PlacementGraph GPU instancer source (follow-up PR)

Paint with Surface Brush, then press **Use cloud in Placement Graph**
in the Surface Brush panel. This creates a separate graph with
`placement.pointcloud` connected to `Layer.domain`. Alternatively,
add **PointCloud** from the graph canvas's Domain menu and wire its
`domain` output into any Layer. `strokeIndex = -1` (default) uses
all painted strokes; `0+` uses one stroke by current list position.

**Each painted point is exactly one instancing candidate**. The density
field can drop a candidate; the Prototypes, Transform and Spacing nodes
then choose a prototype, scale, and constraints just as they do for
grid/mesh/terrain scatter. Candidate StableIDs remain deterministic.
The stored brush points continue following their surface parents:
the editor resolves each UUID and local point against the current scene
transform when the placement preview is regenerated.

The GPU counterpart is `PlacementPointCloudGpu::Upload` (float4
positions and optional normals) feeding `FillScatterParams` and
`PlacementScatterPrograms::pointProgram`; its compute shader
`placement_point_tile.cs.hlsl` uses the **same** density SSA,
prototype selection, random seeds, bounded instance append, constraint
and indirect-draw buffers as the other GPU scatter domains.

**Scope and limits:** The existing PlacementGraph window still renders
a CPU preview of the GPU-evaluable program; this PR adds the GPU
backend domain and an on-device parity test, but does not turn
`place.*` editorSettings into cooked runtime placement assets.
Host applications must register/upload a point-cloud layer and call
the normal scatter/cull/draw chain to render GPU instances.

A cloud is owned by one logical placement tile (by default
`TileID{0,0,0}`), so streaming multiple tiles cannot replicate
it. Hosts that want world-tiled painted clouds must partition the
source into separate per-tile clouds first. The GPU's per-tile limit
is 65,536 points; exceeding it is an explicit error, not a silent
truncation. The brush currently stores positions without surface
normals; imported strokes therefore use the world-up default normal.
Deleting or losing a parent UUID fails the domain binding rather
than placing instances in the wrong object's coordinate system.

**Tests:** `placement_tests` validates exact-point candidate mapping,
tile ownership and GPU argument construction.
`placement_editor_tests` checks graph compilation, wiring and
serialization. `PlacementGpuTests` now has point-cloud kernel vs
CPU-oracle multisets. The on-device test requires a GPU/Vulkan window
and is not part of headless CI.
