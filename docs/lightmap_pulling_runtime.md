# Optional indexed lightmap pulling — runtime integration

## Enabled

- Vulkan initializes the optional chart heap before the global set-0
  descriptors, binds it at storage-buffer binding 7 for every flight,
  and leaves the descriptor stable for in-flight frames.
- AllocateStaticVertexFmt reuses the preallocated heap. No dummy chart
  allocation is made for unlightmapped meshes.
- gfxfiltered.vert.hlsl pulls chart UV and chart ID (8 bytes/vertex)
  while hardware-fetching the 12-byte position and 16-byte shading data.
- The selected LOD's resident chart stream is published via misc.w;
  misc2.x selects its chart transforms and misc2.w is a wrapping delta.
- gfxfilter.mesh.hlsl shares the chart decoder and accepts valid -1 deltas.

## Not enabled

- Direct/fallback vertex shader lightmap fetching (requires explicit
  per-mesh lobe/delta metadata and careful overlap handling with the
  208..240 skinning push tail).
- DXIL shader storage-buffer binding (DXIL path gates out lightmaps).
- Full position/shading pulling, pipeline-family changes or PSO resorting.

## GPU validation required

This draft was authored without a Vulkan/DXC runtime in the environment.
Before merging, compile Vulkan shaders and run with validation enabled:

1. Baked UV1 debug and indexed-vs-mesh rendering image parity.
2. Mix lightmapped and unlightmapped meshes with the same material.
3. All 5/6/9/12 lobe PSOs and GI variant fallbacks.
4. Independently packed negative deltas, especially -1 (0xFFFFFFFF).
5. Skinned objects, LOD sibling selection, scene reload, atlas missing.
6. On NVIDIA, confirm filtered vertex-stage pipeline creation completes.

The configured lightmap heap must fit maxStorageBufferRange for the
single storage-buffer descriptor; an unsupported configuration now
fails during Vulkan initialization with a clear diagnostic.
