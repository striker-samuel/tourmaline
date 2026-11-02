# Editor ImGui ID regression checklist

The ID scopes in the editor use semantic identity rather than visible labels or
sort order. Validate these in a debug build with ImGui's ID-conflict detection
enabled, where supported:

1. Select a node carrying **both** a GI Volume and Audio Room. Edit each
   **Priority** control; confirm that only its own component changes, the
   correct undo command is recorded, and there is no duplicate-ID warning.
2. Add and remove a GI Volume, Audio Room, Camera, Cubemap, Audio Source,
   Trigger Volume, or Export Settings component. Fields elsewhere in the
   inspector must still be editable without changing their labels/IDs.
3. Toggle a chip rendered by `attr::Bool` repeatedly. Its ImGui ID should not
   change when its caption goes between `on` and `off`.
4. Collapse and expand `attr::Vec3`, edit the XYZ values, change another
   component, then return. Disclosure state should stay tied to the field.
5. Expand an outliner item, attach/remove components (which modifies the
   displayed status tags), and verify its expanded state remains unchanged.
   Rename a node and verify its expansion state also persists.
6. In the asset browser, add an asset that sorts before an existing asset.
   Verify existing tile identities do not shift. Test filtering and the
   expanded glTF/.tour views, including their `..` navigation tile.
7. In the placement graph, edit fields on two nodes of the same kind.
   Switching selected nodes should not reuse the previous node's active
   widget or disclosure state.

This checklist is manual until the editor has a headless ImGui window harness.
The full editor build and interactive GPU tests have not been run here.
