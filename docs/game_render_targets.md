# Custom render targets: game API contract (first slice)

This change introduces only the **CPU-side public vocabulary** in
`Engine/Renderer/render_target_selection.h`. It is not yet a functioning
offscreen renderer. No textures, Vulkan images, passes or new bindings are created.

## Proposed game workflow (future integration, NOT executable today)

```cpp
auto target = renderer.CreateRenderTarget({
    .width = 1024, .height = 1024,
    .selection = {.mode = RenderSelectionMode::LayerMask, .layerMask = 2}
});
renderer.SetRenderTargetCamera(target, camera);
renderer.IncludeNode(target, player);
renderer.RequestRender(target);
auto texture = renderer.GetRenderTargetTexture(target);
```

The final implementation should return stable, generation-checked handles,
not Vulkan views or per-frame RenderView indices.

## Selection rules

* `All`: every otherwise-visible renderable World node.
* `LayerMask`: node's render layers intersect target's mask.
* `ExplicitNodes`: only listed handles. Empty list draws nothing.
* In LayerMask mode, an explicitly included handle overrides the mask.
* Exclusions always win.
* A dead/stale World handle must first be rejected using `NodeHandleValid`.
  The helper deliberately does not accept a World reference or pretend that
  checking a handle against an inclusion vector establishes liveness.
* `includeDescendants` is reserved; integration must expand via the current
  World hierarchy and invalidate the expansion when topology changes.
* Layers are separate from per-node `lightLayerMask`. Default should be all
  layers when future per-node render-layer storage is added.

## Renderer integration follow-ups

1. Add renderer-owned target registry, resize/destroy and fence-safe image
   ownership. Validate dimensions, resource budgets and generation reuse.
2. Schedule per-target `RenderViewSet::AllocateCustomView` before upload and
   record offscreen rendergraph passes. Handle its 64-view capacity.
3. Add per-node render-layer metadata and filtered CPU draw-item iteration
   without disturbing the material-sorted draw order.
4. Add GPU Scene view-aware filtering before indirect draw generation; cover
   procedural batch ownership from #331.
5. Expose shader-readable outputs to game UI and materials with explicit
   update and sampling synchronization.
6. Add Vulkan screenshot parity, resize, stereo/multiview, and stale handle
   integration tests across the CPU and GPU Scene raster backends.

The `RenderView` C++/HLSL 256-byte ABI must remain unchanged. Selection lives
alongside the per-view rendering request, not inside the GPU camera struct.
