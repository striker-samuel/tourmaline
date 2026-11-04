# Classic Transpose line (object transforms)

The editor provides an **opt-in** classic ZBrush-inspired three-handle action
line as an alternative to the standard axis/plane/arcball gizmo. This is **not**
a sculptor: no mesh vertices, masks, weights, geometry, or cooked assets are
changed. It only changes the selected scene node's existing local TRS, through
the normal edit command/undo system.

## Enable and disable

- The standard gizmo is the **default**. All existing W/E/R/G transform
  shortcuts and the existing Move/Rotate/Scale buttons remain available.
- Toggle **Classic Transpose line (object only)** in the View menu, or use the
  **Transpose** toggle beside the transform buttons.
- Switch back off at any time between drags to restore the standard gizmo.
- **Reset Transpose line** in the View menu repositions it on the selected
  object's pivot. Selecting another node also starts a fresh line.

## Interaction

- **Red handle:** drag to reposition the entire line, without modifying the
  object. This can place the pivot at a joint or any convenient reference.
- **Yellow middle handle:** drag to transform the node in the current W/E/R
  mode. Move uses free camera-plane motion; Rotate arcs around the red pivot;
  Scale is uniform around that pivot.
- **Blue tip:** drag to translate along the line axis, or rotate about the
  line axis (in Rotate mode). In Scale mode, it applies uniform scaling.
- **Ctrl + blue tip:** reposition the far endpoint of the line without
  transforming the object (change its direction and length).
- **Shift while transforming:** use existing translate/rotate/scale snapping.
- **Alt + mouse:** keeps the editor's existing camera navigation behavior.
- Release the mouse after a transform to commit **one undoable edit**.

## Limitations

- The line is session-only editor state. Transform edits persist through
  existing scene authoring; the visual line position does not get serialized.
- This first version operates on the **primary selected scene node** (not a
  full multi-selection group). The ordinary gizmo retains its existing
  multi-selection behavior.
- No vertex deformation or sculpt/mesh editing is intended or required.
- End-to-end viewport operation still needs manual GPU-enabled editor testing.
