# Lightmap vertex pulling — current Vulkan runtime

## Enabled on Vulkan

- The optional packed chart-vertex heap is allocated before global descriptors,
  is bound at set 0 / binding 7 for each flight, and remains descriptor-stable
  while frames are in flight.
- The indexed `gfxfiltered.vert.hlsl` uses hardware position/shading fetch,
  pulling only chart UV and chart ID (8 bytes per vertex) for selected LODs
  with resident baked streams. The mesh shader shares the packed decoder.
- A **separate** `default_lightmap.vert.hlsl` shader enables opt-in
  lightmaps for eligible static direct opaque/ProjectLit/A2C and ordinary
  transparent draws (merged in #53). The regular `default.vert.hlsl`
  remains unchanged and performs no lightmap fetch on unbaked draws.
- Chart transforms are indexed by the selected LOD's scene-table offset.
  Direct metadata is pushed at bytes 192–208 and 224–240, after the skin
  tail. Both direct and filtered paths preserve valid wrapping -1 deltas
  (`0xFFFFFFFF`) separately from the absent-stream sentinel.
- Skinned meshes do not use baked lightmaps. Far vertex-lit, specialized
  transparent, wireframe, and debug routes keep their existing behavior.

## Scene changes and failure behavior

- Each chart-table rebuild receives a process-unique revision; the GPU upload
  cache checks world identity and revision so a new scene cannot reuse stale
  chart transforms from the old one.
- Changed chart tables use a fresh GPU buffer instead of overwriting data
  that earlier GPU frames may still read. Old resource holders retire safely.
- If the chart-transform buffer is unavailable, lightmap eligibility fails
  closed for both direct and filtered draws; the renderer uses dynamic GI
  instead of dereferencing a null chart-transform device address.
- The lightmap heap must fit the Vulkan device's `maxStorageBufferRange`.

## Not enabled

- The equivalent DXIL/D3D12 lightmap storage descriptor and direct shader
  integration are not implemented. DXIL lightmap shader paths remain gated off.
- Full position/shading pulling, new material families, and skinned baked
  lightmaps are not part of this work.

## Validation

The CPU regression in `world_entity_tests` checks distinct chart-table
revisions across two worlds and repeated rebuilds. Full shader compilation
and GPU rendering were not performed while authoring this fix.

On a Vulkan GPU, additionally test scene reloads with different atlas charts,
LOD switching, mixed lightmapped/unlightmapped static and skinned geometry,
masked A2C / ProjectLit / transparent variants, chart-buffer creation failure,
and device validation warnings. Compare indexed, mesh, and direct UV1 rendering.
