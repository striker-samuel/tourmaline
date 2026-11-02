# World2DRenderer — instanced sprite pipeline

\`tour::World2DRenderer\` is an **independent** 2D implementation of
\`engine::IRenderer\`, not a mode of the 3D \`WorldRenderer\`. It uses the
existing Vulkan/RHI rendergraph, shader package, bindless textures, and
frame-in-flight buffers.

### Typical host integration

\`\`\`cpp
tour::World2DRenderer sprites;
// after ResourceManager initialization:
const uint8_t white[4] = {255, 255, 255, 255};
uint32_t whiteId = sprites.Atlas().Insert(1, 1, white);
if (!sprites.UploadAtlas(ctx) || !sprites.EnsureShaders(ctx)) {
    // handle failure before recording frames
}
// each acquired/fenced frame, after rg.NewFrame():
sprites.BeginFrame();
sprites.SetCamera({0, 0}, 1.0f); // top-left and pixels/world unit
tour::Sprite2D sprite{};
sprite.region = whiteId;
sprite.position = {320, 200}; // CENTER, world-space; Y points down
sprite.size = {64, 64};
sprite.z = 2.75f;
sprites.Submit(sprite);
bool ok = sprites.AppendPass(ctx, rg, colorView, extent, flightSlot);
// caller executes rendergraph and presents.
// before destroying rg/ctx:
sprites.Destroy(rg);
\`\`\`

### Invariants and tradeoffs

- **Float Z is retained** for every sprite (not integer layers). Ascending
  Z is drawn back-to-front, stable for equal Z. Blended sprites must **not**
  be globally sorted by atlas page: instead contiguous runs sharing the
  same page use a single \`Draw(6, N, 0, firstInstance)\`. The shader maps Z
  to clip range but depth testing/writes are disabled for alpha correctness.
- **Atlas**: deterministic multi-page RGBA8 shelf pack; one texel of
  duplicated edge padding. Regions stay stable across inserts. Pages are
  currently fixed-size, uncompressed, and contain no mips. Source bytes
  are raw RGBA8 (linear); load/decode images separately. Empty/oversized
  images are refused with \`kInvalidSprite2D\`. Atlas changes require an
  explicit \`UploadAtlas\` before drawing; upload recreates the pages and
  the RHI defers old GPU resources. Insert all static sprites first.
- **Instance buffer**: CPU sorts into a single packed 56-byte-per-sprite
  stream. One staging upload per active flight frame, geometric capacity
  growth, and reuse keyed by the **fenced flight slot**, not total frame
  number. One draw per consecutive page run, six shader-generated vertices
  per instance; no per-sprite push constants, vertex buffers or descriptors.
- **Resources**: shaders load through the engine shader library (including
  deployed engine-shaders.pak). The host owns attachments, viewport format,
  graph execution, and synchronization. Call \`Destroy(rg)\` before the graph
  is torn down. \`AppendPass\` assumes exactly one call per fenced flight
  frame and a consistent ResourceManager/RenderGraph pair.
- **Blending**: straight-alpha blending, with \`RGBA8_UNorm\` atlas and
  linear color tint. A host using sRGB art should convert source pixels to
  linear or extend the atlas format; blending/order must be reviewed if
  adding opaque-only depth paths, pixel snapping, clipping, or texture arrays.

CPU-only regression: \`world2d_tests\` checks page overflow/padding,
invalid inputs, stable float-Z ordering, and page-run boundaries.
