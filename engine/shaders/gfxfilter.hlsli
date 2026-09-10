// Shared GeometryFX-filter structs (Vulkan port, per-cluster).
// Included by gfxfilter.cs.hlsl (compute) and gfxfiltered.vert.hlsl.
// C++ mirrors: Engine/Renderer/gfx_filter.h (static_asserted sizes).

#pragma once

// Must match C++ smvk::gfx::CullFlagBits.
#define GFX_CULL_DEGENERATE  0x1
#define GFX_CULL_BACKFACE    0x2
#define GFX_CULL_FRUSTUM     0x8
#define GFX_CULL_SMALLPRIM   0x20
#define GFX_CULL_OCCLUSION   0x40 // T4: HZB sphere test (facing-agnostic)
#define GFX_CULL_CONTRIBUTION 0x80 // T4: footprint below minContribution
#define GFX_CULL_FLIPWINDING 0x100

// Conservative culling guard band (px, internal resolution) for the main
// view. The SMAA T2x projection jitter swings clip edges by +/-0.25 px per
// frame; exact boundary tests would cull edge clusters/triangles on
// alternating frames (visible popping). The guard only ever keeps more
// (never punches holes): the frustum test widens, the occlusion footprint
// covers more texels (all must occlude), the contribution area grows.
// Mirrors kMainViewCullGuardPx (app) and the RenderView plane dilation.
#define GFX_CULL_GUARD_PX 1.0f

#include "hzb.hlsli"

#define GFX_THREADS_PER_CLUSTER 128
#define GFX_MAX_DRAWS 4096
#define GFX_MAX_BATCHES 65536
// Explicit bounds (this DXC rejects unsized struct members; mirrors the
// C++ capacities in Engine/Renderer/gfx_filter.h).
#define GFX_MAX_VERTICES 44739242   // 512MB pos heap / 12B (see mesh.cpp)
#define GFX_POS_WORDS 134217726     // GFX_MAX_VERTICES * 3 (uint view below)
#define GFX_MAX_CLUSTERS 262144
#define GFX_MAX_FILTERED (65536 * 128 * 3)

// Positions as uint words (3 per vertex): the heap is packed float3
// (stride 12), which a float3 array view cannot express under block layout
// rules (member-array stride must align to 16). Load via asfloat.
struct GfxPositions { uint words[GFX_POS_WORDS]; }; // static pos heap (global ids)
struct GfxIndices   { uint idx[GFX_MAX_FILTERED]; };   // cluster-ordered global ids (owned)
// Per cluster: a = (indexOffset, triCount, 0, 0), b = (center.xyz, radius).
struct GfxClusterData { uint4 a; float4 b; };
struct GfxClusters  { GfxClusterData cluster[GFX_MAX_CLUSTERS]; };

// One uint4 per batch: (clusterIndex, drawIndex, outputOffset, unused).
struct GfxBatches { uint4 batch[GFX_MAX_BATCHES]; };

#define GFX_DRAW_TWOSIDED 0x1 // misc.y: double-sided, skip backface cull

// Draw-set buckets (T1; mirrors C++ DrawSetBit_*). The seed depth pass
// issues one PSO per bucket; views mask which buckets they accept.
#define GFX_DRAWSET_OPAQUE 0x1
#define GFX_DRAWSET_MASKED 0x2
#define GFX_DRAWSET_ALL (GFX_DRAWSET_OPAQUE | GFX_DRAWSET_MASKED)

// Temporal visibility list (T1 state; consumed by the seed pass in T3).
// One uint2 per surviving (cluster, draw): xy = (clusterIndex, drawIndex).
// Draws resolve through GfxDraws (model matrices); clusters through
// GfxClusters + GfxIndices (cluster-ordered global vertex ids).
#define GFX_MAX_VISIBLE 65536 // mirrors kMaxBatches
struct GfxVisible { uint2 vis[GFX_MAX_VISIBLE]; };

struct GfxDraw
{
    float4x4 model;
    // x = mesh index, y = flags (GFX_DRAW_*), z = draw-set bucket
    // (GFX_DRAWSET_*); uint4 keeps 16B alignment under block rules.
    uint4 misc;
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
    uint2 pad0; // 8 bytes: keeps viewProj 16B-aligned (raw uint64 would emit Capability Int64)
    float4x4 viewProj;
    float3 eye;
    float pad1;
    uint cullFlags;
    uint width;
    uint height;
    uint batchCount;
    uint2 pad2;
    // T4 occlusion/contribution inputs (0/empty disables the tests).
    vk::BufferPointer<HzbBuffer> hzb;
    uint hizWidth;
    uint hizHeight;
    uint hizMips;
    float minContribution; // px^2 footprint threshold (0 = off)
    // Filtered-output region base (uint32 indices into the static index
    // monoblock) for this dispatch's flight slice. Indirect firstIndex is
    // written as indexBase + outputOffset (absolute), so draws address the
    // monoblock directly and the index binding never switches buffers.
    uint indexBase;
    uint pad3;
};
//static_assert(sizeof(GfxFilterPush) == 200);
