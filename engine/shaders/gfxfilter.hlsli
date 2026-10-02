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
#define GFX_CULL_HZBSKIPMASKED 0x200 // T4: exclude MASK draws from the HZB test

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
#define GFX_MAX_MESHES 8192         // mirrors kMaxMeshes
#define GFX_MAX_TEXTURES 8192       // mirrors smvk::MAX_TEXTURES

// Positions as uint words (3 per vertex): the heap is packed float3
// (stride 12), which a float3 array view cannot express under block layout
// rules (member-array stride must align to 16). Load via asfloat.
struct GfxPositions { uint words[GFX_POS_WORDS]; }; // static pos heap (global ids)
struct GfxIndices   { uint idx[GFX_MAX_FILTERED]; };   // cluster-ordered global ids (owned)
// Per cluster: a = (indexOffset, triCount, 0, 0), b = (center.xyz, radius).
struct GfxClusterData { uint4 a; float4 b; };
struct GfxClusters  { GfxClusterData cluster[GFX_MAX_CLUSTERS]; };

// One uint4 per batch: (clusterIndex, drawIndex, outputOffset, flags).
// flags bit 0 (GFX_BATCH_DEMAND): this group is the DESIGNATED PUBLISHER
// for its draw's texture demand — the first batch emitted for the draw
// in this view's material-sorted expansion (BuildBatches sets it).
// Every group of a draw shares the draw's material, so exactly one
// designated group publishes per draw instead of one publication per
// surviving cluster; see PublishDrawDemand for the containment argument
// (draw sphere contains every cluster sphere -> a designated group that
// skips publishing implies no sibling group survives either).
struct GfxBatches { uint4 batch[GFX_MAX_BATCHES]; };
#define GFX_BATCH_DEMAND 0x1 // batch.w bit 0

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
    // (GFX_DRAWSET_*), w = lightmap chunk (0xFFFFFFFF = none; passed flat
    // to the lit fragment for the static/dynamic GI separation).
    uint4 misc;
    // x = base offset into the scene-wide chart transform buffer for this
    // draw's mesh (0 when the mesh carries no lightmap); y = material id
    // (the designated publisher's PublishDrawDemand resolves the 5 texture
    // slots through the materials buffer); z,w reserved.
    // The VS resolves the atlas UV as
    // chartUv * xforms[misc2.x + chartId].xy + xforms[misc2.x + chartId].zw.
    uint4 misc2;
};
struct GfxDraws { GfxDraw draws[GFX_MAX_DRAWS]; };

// Scene-wide lightmap chart atlas transforms (one float4 per mesh chart:
// xy = scale, zw = offset; atlasUv = chartUv * xy + zw). Concatenated
// sorted by mesh then chart; each draw's misc2.x is its mesh's base.
// Sized generously: the runtime uploads exactly the cooked count.
#define GFX_MAX_CHART_XFORMS 1048576
struct GfxChartXforms { float4 xform[GFX_MAX_CHART_XFORMS]; };

// Resolves the atlas-space lightmap UV for one vertex. chartId ==
// 0xFFFFFFFF (uncharted) yields 0 — the fragment gates on the draw's
// lobe count anyway, so the sample lands in the atlas margin.
float2 GfxLightmapUv(float2 chartUv, uint chartId, uint chartOffset,
    vk::BufferPointer<GfxChartXforms> xforms)
{
    if (chartId == 0xFFFFFFFFu)
        return float2(0.0f, 0.0f);
    float4 xf = xforms.Get().xform[chartOffset + chartId];
    return chartUv * xf.xy + xf.zw;
}

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

// Per-mesh union bound over its clusters (xyz center, w radius, OBJECT
// space — the publisher transforms it by the draw's model exactly like
// ClusterOccluded does with cluster bounds). Static, load-time upload.
struct GfxMeshBounds { float4 centerRadius[GFX_MAX_MESHES]; };

// Draw-level texture-demand source (replaces the retired per-fragment
// TS_ReportUsage feedback at ~1/1000 the publication traffic):
//
//   visible draw (designated publisher group)
//     -> material id (GfxDraw.misc2.y)
//     -> MaterialValues tex/tex2.x = 5 bindless slots
//     -> conservative projected rect (same math as ClusterOccluded, but on
//        the mesh sphere — containment guarantees a draw-culled bound
//        implies every cluster bound culled, so skipping publication is
//        never a missed demand, only an over-request)
//     -> per-slot wanted FULL-CHAIN mip (mirror of
//        TextureStreamer::WantedMipForScreenCoverage, incl. mipBias +
//        clamp to fullMips-1)
//     -> InterlockedMin(demand[slot], want) — one uint per bindless slot,
//        0xFFFFFFFF = "no draw reported this texture this frame".
//
// The CPU consumes the ring slice every 4th frame (batched with the
// exposure/DS readback ticket), skips 0xFFFFFFFF, and forwards via
// TextureStreamer::RequestMip(slotMap[slot], wantedFullMip, frame) —
// full-chain mips keep residency state out of this shader entirely.
struct GfxTextureMeta
{
    uint longSide;  // max(baseWidth, baseHeight) texels
    uint fullMips; // 0 = slot holds no streamed texture (skip)
};
struct GfxTextureMetaTable { GfxTextureMeta meta[GFX_MAX_TEXTURES]; };
struct GfxDemand { uint demand[GFX_MAX_TEXTURES]; };

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
    // Draw-level texture-demand inputs (0/empty disables the publisher in
    // the shader; PushAndDispatch zero-gates the whole set on any null
    // BDA — same belt-and-braces rule as the HZB inputs above).
    vk::BufferPointer<GfxMeshBounds> meshBounds;      // per-mesh union bounds
    vk::BufferPointer<MaterialsBuffer> materials;    // THIS frame's slice
    vk::BufferPointer<GfxTextureMetaTable> textureMeta; // per-bindless-slot meta
    vk::BufferPointer<GfxDemand> textureDemand;      // this flight's ring slice
    uint textureMetaCount; // live slot count (upper bound for slots/atomics)
    float mipBias;        // mirrors TextureStreamerConfig::mipBias
};
//static_assert(sizeof(GfxFilterPush) == 240);
