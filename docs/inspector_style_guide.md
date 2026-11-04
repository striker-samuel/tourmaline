# Inspector UI style guide

This is the default convention for all new or revised Tourmaline inspector controls.

## Layout
- Every editable property is **one row**: a label on the left and a control on the right.
- The left edge of **every value/control** is the same vertical line, including Name, Translate X/Y/Z, Rotate X/Y/Z, Scale X/Y/Z, flags, enums, and read-only properties.
- Use the shared `editor::attr` helpers in `editor/src/editor_attr_ui.h` (`LabelCell`, `Float`, `Check`, `Text`, etc.) rather than hand-positioning fields.
- Size the value field to the remaining row width. Long labels should clip inside the label column and show the full name on hover.
- Vector3 editing is **three rows**, with names such as `Translate X`, `Rotate Y`, `Scale Z`. Do not add a separate parent/summary row or nest/indent the axis fields.
- Flags are a **single label + on/off control** row (for example `Static [ ]`). Prefer short names; detailed scope is a tooltip, not a giant label.
- Do not add a separator between adjacent properties. Use separators only to mark a meaningful group boundary.
- Do not show repeated `Effective` lines or explanatory disabled text beneath an editable control. Move descriptions, inheritance details and units to tooltips where they are useful.

## Interaction
- Preserve stable ImGui IDs across frames and distinct IDs for every axis and repeated flag.
- Preserve command/undo semantics, drag activation/finish aggregation, and finite-number validation.
- Do not hide editable controls behind disclosures just to compensate for narrow inspector widths.
- Do not replace useful status or warnings with a tooltip when they need immediate attention.

## Example
```text
Name              [node9]
Translate X       [0.000]
Translate Y       [0.000]
Translate Z       [0.000]
Rotate X          [0.000]
Rotate Y          [0.000]
Rotate Z          [0.000]
Scale X           [1.000]
Scale Y           [1.000]
Scale Z           [1.000]
Static            [off]
Bake lightmaps    [off]
Exclude lightmaps [off]
Dither closeup    [off]
Volumetric shadow [off]
```

## Review checklist
1. Check narrow and wide inspector windows, including high-DPI scaling.
2. Verify the label/value column stays aligned across control types.
3. Verify each transform axis changes the correct component and creates a single undoable gesture.
4. Verify toggles still update their original scene flags.
5. Verify no redundant dividers, permanently disabled explanation lines, or duplicate effective readouts were introduced.
