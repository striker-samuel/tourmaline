# Animation clip preview in the editor

The asset browser's Inspector recognizes `.tourAnim` animation documents and
loads the complete document (not the generic 64 KiB text preview).

1. Select a `.tourAnim` asset in the Assets panel. The Inspector lists its
   named clips, duration, authoring FPS, source reference, and individual joint
   tracks with channel, interpolation, and key counts.
2. Select a `.tour` prefab in the Assets panel and choose **Edit as prefab
   world...**. The prefab must contain a skinned mesh/skeleton.
3. Re-select the animation asset, choose the clip, and click **Preview clip
   in prefab world**. The inspector keeps animation assets read-only.
4. In the Prefab World pane, use **Play/Pause**, **Restart**, **Loop**,
   **Speed**, and the **Time** slider. Moving the slider pauses playback,
   making it easy to inspect a particular pose.
5. **Stop preview** restores the original local joint transforms in the
   prefab world. Closing or opening a different prefab also discards the
   temporary preview state.

The existing separate prefab camera/renderer is used for 3D previews. Clip
playback does not write `.tour` commands, modify the parent world, or save
the animation into the prefab. The animation's cluster-local joint IDs must
match the source skeleton; a mismatched animation cannot be automatically
retargeted. When multiple skeletons are present, the first skinned cluster
is used.

## Manual verification

- Select a multi-clip `.tourAnim`: all clips and tracks appear, even when
  the file exceeds the generic preview limit.
- Select a malformed animation: the parse error is shown without crashing,
  with a retry control.
- Open a skinned `.tour` in prefab mode, pick an animation clip, and check
  moving poses, looping, speed changes, pause, and scrubbing backwards.
- Stop preview and verify the bind/rest pose returns, with no new prefab
  undo entries or parent-scene edits.
- Select an animation without a prefab open: the inspector provides setup
  guidance instead of attempting to render without a skeleton.
- Select an ordinary text or binary asset: the generic inspector remains
  unchanged.
