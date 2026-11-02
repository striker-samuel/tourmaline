# Lightmap pulling: scaffold-only follow-up

**Depends on:** scaffold/vertex-pulling-contracts-20261102 (PR #9).

Adds CPU-only validation and opt-in HLSL helpers for chart UV decoding and
optional, independently allocated lightmap vertex windows.

## Files

- `engine/include/Engine/Renderer/lightmap_pull.h`: presence, lobe-class,
  checked chart indices, vertex-window/byte-bound mapping.
- `engine/shaders/lightmap_pull.hlsli`: unpack RG16_UNORM chart UV,
  load two packed words from a caller-provided ByteAddressBuffer.
- `tests/test_lightmap_pull.cpp`: GPU-less edge-case regression test.

## Critical contract

The current GPU draw's `misc2.w` delta may legitimately equal
`0xFFFFFFFF` (-1 modulo uint32) when the lightmap stream's independently
packed base is one vertex before the primary mesh base.

Do **not** treat that delta as an absent-stream sentinel. The eventual
runtime implementation must use an explicit validity/residency bit or a
rigorously validated existing per-draw state. Existing mesh-shader code
currently checks the delta against that sentinel; fixing it is part of
the later behavior-changing integration, NOT this scaffold.

Chart ID `0xFFFFFFFF` means uncharted. The current
`GfxLightmapUv` returns zero for it; validation of chart completeness
must be done before a lit draw is marked eligible.

## Not wired yet

This PR does NOT:
- create or bind a descriptor for the global lightmap heap;
- include the HLSL helpers from an active vertex/mesh shader;
- modify `GfxDraw`, push constants or material range sorting;
- restore atlas UV output in `gfxfiltered.vert.hlsl` or `default.vert.hlsl`;
- change SG-lightmap sampling or atlas upload;
- claim visual parity or lightmap rendering correctness.

## Next implementation

1. Audit Vulkan set 0 / D3D12 descriptor ABI; consider global binding 7.
2. Publish read-safe stream residency and generation/state for all draws.
3. Wire indexed VS optional chart fetch using `SV_VertexID + misc2.w`.
4. Share decode with the mesh shader and repair delta sentinel checks.
5. Handle direct-lit per-mesh metadata and overlapping skin push tails.
6. Run GPU parity checks including UV1, GI variants, LOD and scene reload.

## Validation

`LightmapPullTests` runs without an RHI. No DXC/GPU execution yet.
