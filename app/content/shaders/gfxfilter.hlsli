// Shared GeometryFX-filter structs (Vulkan port, per-cluster).
// Included by gfxfilter.cs.hlsl (compute) and gfxfiltered.vert.hlsl.
// C++ mirrors: Engine/Renderer/gfx_filter.h (static_asserted sizes).

#pragma once

// Must match C++ smvk::gfx::CullFlagBits.
#define GFX_CULL_DEGENERATE  0x1
#define GFX_CULL_BACKFACE    0x2
#define GFX_CULL_FRUSTUM     0x8
#define GFX_CULL_SMALLPRIM   0x20
#define GFX_CULL_FLIPWINDING 0x100

#define GFX_THREADS_PER_CLUSTER 128
#define GFX_MAX_DRAWS 1024
#define GFX_MAX_BATCHES 4096
// Explicit bounds (this DXC rejects unsized struct members; mirrors the
// C++ capacities in Engine/Renderer/gfx_filter.h).
#define GFX_MAX_VERTICES 44739242   // 512MB pos heap / 12B (see mesh.cpp)
#define GFX_MAX_CLUSTERS 262144
#define GFX_MAX_FILTERED (4096 * 128 * 3)

struct GfxPositions { float3 pos[GFX_MAX_VERTICES]; }; // static pos heap (global ids)
struct GfxIndices   { uint idx[GFX_MAX_FILTERED]; };   // cluster-ordered global ids (owned)
struct GfxClusters  { uint4 cluster[GFX_MAX_CLUSTERS]; }; // xy = (indexOffset, triCount)

// One uint4 per batch: (clusterIndex, drawIndex, outputOffset, unused).
struct GfxBatches { uint4 batch[GFX_MAX_BATCHES]; };

struct GfxDraw
{
    float4x4 model;
    uint mesh;
    uint3 pad;
};
struct GfxDraws { GfxDraw draws[GFX_MAX_DRAWS]; };

struct GfxFiltered { uint idx[GFX_MAX_FILTERED]; };

struct GfxIndirectCmd
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct GfxIndirect { GfxIndirectCmd cmds[GFX_MAX_BATCHES]; };

struct GfxFilterPush
{
    vk::BufferPointer<GfxPositions> positions;
    vk::BufferPointer<GfxIndices>   indices;
    vk::BufferPointer<GfxClusters>  clusters;
    vk::BufferPointer<GfxBatches>   batches;
    vk::BufferPointer<GfxDraws>      draws;
    vk::BufferPointer<GfxFiltered>   filtered;
    vk::BufferPointer<GfxIndirect>   indirect;
    uint64_t pad0;
    float4x4 viewProj;
    float3 eye;
    float pad1;
    uint cullFlags;
    uint width;
    uint height;
    uint batchCount;
    uint2 pad2;
};
