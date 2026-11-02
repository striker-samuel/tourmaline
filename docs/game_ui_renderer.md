# GameUiRenderer — textured rectangles and simple HUD layout

\`tour::GameUiRenderer : engine::IRenderer\` is a **separate**, lightweight
rendergraph renderer for in-game UI. It is intentionally unrelated to the
full ImGui/editor UI pass and the world-space \`World2DRenderer\`.

- Pixel coordinates, top-left origin, Y down (no camera).
- Solid rectangles and textured rectangles with per-rect UV subranges,
  tint, straight alpha and optional nested clip rectangles.
- A **single instanced draw** for mixed textures: each instance carries
  its own bindless texture index; no texture-group reordering.
- A GPU-independent \`UiLayout2D\`: nested row/column flow, padding,
  gaps, cross-axis alignment, fill remaining space, anchors and clipping.
- 72 bytes per rectangle, reusable per-fenced-frame instance buffers.
  Textures belong to the host. No font shaping, input routing, scrolling,
  or retained widget lifecycle in this first cut.

## Sample

\`\`\`cpp
tour::GameUiRenderer ui;
// Each acquired/fenced frame, after rg.NewFrame(), BEFORE rg execution:
ui.BeginFrame();
tour::UiLayoutStyle2D column{};
column.padding = {16, 16, 16, 16}; // left, top, right, bottom
column.gap = 8;
tour::UiLayout2D layout({{0, 0}, {1280, 720}}, column);

auto header = layout.Next({0, 64}); // 0 width = stretch
ui.DrawRect({.bounds = header.bounds,
             .tint = {0.06f, 0.07f, 0.1f, 0.9f},
             .clip = header.clip});

tour::UiLayoutStyle2D row{};
row.direction = tour::UiLayoutDirection2D::Row;
row.align = tour::UiLayoutAlign2D::Center;
row.padding = {12, 8, 12, 8};
row.gap = 12;
layout.Push(header, row);
auto icon = layout.Next({48, 48});
ui.DrawRect({.bounds = icon.bounds,
             .texture = iconTexture,  // an existing smvk::TextureHandle
             .clip = icon.clip});
layout.Pop(); // discard nested row, resume root column

// After the world/scene passes, on the same host color attachment.
// frameSlot is the acquired/fenced flight slot, NOT total frame count.
bool ok = ui.AppendPass(ctx, rg, colorView, {1280, 720}, frameSlot);
// host executes graph / presents; before destroying ctx/rg:
ui.Destroy(rg);
\`\`\`

\`UiLayout2D::Anchor(parent, size, UiAnchor2D::BottomRight, offset)\`
is useful for responsive HUD overlays (health bars / minimaps) that do
not participate in a row/column flow. A \`UiLayoutItem2D\` contains both
its bounds and inherited clip region; pass **both** to \`DrawRect\`.
Use \`UiRect2D::Contains\` plus the clip for hit-testing in game code.

## Resource / ordering contract

The host owns textures and upload synchronization. The renderer looks up
bindless indices from live \`smvk::TextureHandle\` values each frame;
an empty texture handle means a solid rectangle. UV subranges allow
texture atlas regions without additional pipelines. The renderer does
not own an atlas and does not modify textures.

Submit UI **after** world drawing and before presentation; its pass loads
the existing color by default (\`clear = false\`). Everything stays in
submission/painter order, so mixed alpha panels/textures composite
correctly. The shader discards fragments outside an intersected clip,
avoiding per-rectangle Vulkan scissor changes. This is a draw-only
renderer, not a full retained widget framework.

CPU-only tests run via \`ctest -R game_ui_tests\`; GPU shader compilation,
rendergraph integration and screenshots still need a graphics smoke test.
