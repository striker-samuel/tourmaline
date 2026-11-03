# Direct/fallback lightmap pulling — static geometry only

Stacked on the indexed runtime PR; this does not expand the material,
vertex-input or SG PSO families.

## Direct draw implementation

The DefaultLit direct VS reads the same Vulkan set-0, binding-7 optional
chart heap as the GPU-driven indexed VS. The selected LOD's mesh publishes:

- UvFlatVsPushTail at bytes 192..208: chart transform base + BDA.
- DirectLightmapPullTail at bytes 224..240: independent heap delta,
  cooked SG count and opt-in magic.
- DepthSkinTail is pushed first at 208..240; the second tail replaces
  **only its padding**, never the skinned/deformed input words.

Both direct tails are pushed for **every** draw to prevent stale state.
The vertex shader verifies the magic and stream/lobe presence before
loading. It also refuses lightmaps when the draw is skinned, even if
the tail is accidentally nonzero.

This keeps lightmaps strictly static. Animated geometry always uses
regular GI sources.

## Direct-only culling fallback

EnsureChartXforms is run before Main even if the GPU-driven filter did
not create a slot. It caches the GPU table by world chart-version.

## Not included

- DX12 optional lightmap storage SRV support; DXIL retains no-lightmap
  behavior until that platform binding is implemented.
- Full position/shading vertex pulling, material-family changes, cooked
  asset migrations or changes to GPU-skinned geometry.

## Validation required

The branch is unverified without DXC and a Vulkan GPU. Build and run:

1. Direct-render fallback and all existing Main non-filterable meshes.
2. Dynamic skinned + baked static objects interleaved by material.
3. A selected LOD with an independently allocated chart stream.
4. GPU-driven indexed VS vs direct VS UV1 and irradiance parity.
5. Scene change/reload, chart atlas off, and absent chart data.
6. Vulkan validation with MSAA, transparent and ProjectLit materials.
7. Confirm bytes 224..240 are correctly owned through shader reload.
