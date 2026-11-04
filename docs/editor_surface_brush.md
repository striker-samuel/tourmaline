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
