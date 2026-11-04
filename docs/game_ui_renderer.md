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

## Authored SpriteSet atlases and nine-slice panels

The first-class `.spriteset` catalog already provides named pixel rectangles,
grids, animations, trim offsets and nine-slice border metadata. GameUI **does
not** repack these textures: use an existing, host-owned sampled texture matching
the SpriteSet's declared dimensions. This avoids duplicating the separate
`World2DRenderer` runtime shelf packer.

```cpp
// Obtain sprites metadata via ParseSpriteSetText / ProjectRuntime::ReadAsset,
// and load the image once using the application's existing texture manager.
ui.BeginFrame();
ui.DrawSprite(sprites, "icon", {{20, 20}, {32, 32}}, atlasTexture);
ui.DrawNineSlice(sprites, "panel", {{10, 80}, {420, 140}},
                 atlasTexture, glm::vec4(1.0f), panelClip);
```

`DrawSprite` resolves the named atlas region and renders it using the existing
single-draw bindless rectangle batch. If the sprite is trimmed, the visible
portion is placed within its original untrimmed bounds; the application retains
responsibility for any pivot/animation behavior.

`DrawNineSlice` splits the authored region into up to nine consecutive
rectangles; fixed-pixel corners and border edges are preserved and the center
stretches. If the target is smaller than the sum of opposite borders, those
destination borders shrink proportionally, leaving no negative center area.
Zero-width or height source/destination strips are skipped. Validation and
instance-capacity checks happen before drawing, so failure cannot submit a
partial panel. No new shader or draw call is required. Call order, tint, clip
and the original host-owned bindless texture are preserved.

The atlas remains **pre-authored**. Automatic source-image packing, runtime
image creation, padding/extruded gutters and mip generation are outside this
API. When bilinear filtering or mipmapping atlas art, the asset author should
provide gutters between regions to prevent sampling adjacent sprites.
For pixel-art sheets, `SetPixelArtSampling(true)` chooses nearest sampling.

The functions return `false` for missing names, invalid bounds/metadata,
empty texture handles and exhausted rectangle capacity. Existing direct
`DrawRect` and `SpriteSet::UV` usage remain valid.

CPU tests: `ctest -R 'game_ui_tests|spriteset_tests' --output-on-failure`.
A Vulkan smoke test should additionally cover linear filtering near neighboring
atlas regions, asymmetric borders, very small panels, clipping, alpha order,
reloaded textures and HiDPI coordinate mapping.
