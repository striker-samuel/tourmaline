// Shared mesh-shader gfx filter structs (task-less mesh path).
// Included by gfxfilter.mesh.hlsl. C++ mirrors: Engine/Renderer/gfx_filter.h
// (GfxMeshParams, static_asserted sizes).
//
// Path shape (slice 1): ONE mesh workgroup per cluster HALF —
// DispatchMeshTasks(2 * batchCount): workgroup bx covers batch bx/2,
// half bx%2. 64 threads, one triangle each; halves are <= 64 tris so the
// 3-verts-per-tri emission (<= 192 verts, <= 64 prims) fits the
// 256-vertex mesh output limit with NO dedup hash. Culled workgroups emit
// zero outputs (SetMeshOutputCounts(0, 0)).
//
// Eligibility contract (EXACT on both sides, or draws double/drop):
// a batch is mesh-drawn iff draw.drawSet == OPAQUE && !twoSided &&
// !giActive && !(lobe in {5,6,9,12} with the sg twin built). The CPU
// skips exactly those material ranges in the indexed loop; the mesh
// shader gates each batch on the same predicate from the same inputs
// (draw record + params giActive/sgTwinMask). See GfxMeshEligible.

#pragma once

#include "gfxfilter.hlsli"

// Mesh workgroup geometry: 32 threads (ONE wave — WavePrefixCountBits
// is wave-scoped, so a 64-lane group would restart compaction mid-group
// and overwrite slots), 2 triangles per lane, halves of <= 64 tris.
// Output <= 64 tris / 192 verts fits the 256-vertex limit, no dedup.
#define GFX_MESH_THREADS 32
#define GFX_MESH_TRIS_PER_LANE 2
#define GFX_MESH_MAX_TRIS 64
#define GFX_MESH_MAX_VERTS (64 * 3)
#define GFX_MESH_HALVES 2

// Positions heap as uint words (same GfxPositions view as the filter).
// Shading heap as uint words (6 per vertex: color, uv, chartUv, normal,
// tangent, chart — see VertexShading in Engine/Renderer/mesh.h).
struct GfxShadingHeap { uint words[GFX_MAX_VERTICES * 6]; };
// Compacted eligible batch indices for one variant dispatch.
struct GfxMeshBatchList { uint batch[GFX_MAX_BATCHES * 2]; };

// Per-frame mesh params (one small device buffer, BDA in the 224 tail,
// uploaded ONCE per RecordMeshMain — never per span: per-span uploads to
// the same buffer race (transfer-queue writes vs graphics reads across
// dispatches in one pass). Per-dispatch slicing rides the tail push
// (listBase/listCount below), which is per-dispatch by construction.
// The CPU partitions eligible batches per FS variant (base/sg/gi, exactly
// the indexed leg's program selection), so the shader trusts its list —
// no eligibility predicate here.
struct GfxMeshParamsData
{
    // viewProj FIRST: 16-aligned at offset 0 on both sides (a pointer
    // run before it would leave offset 56, padded differently by HLSL
    // vs C++ packing — the static_assert below guards the whole blob).
    float4x4 viewProj;
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<GfxClusters> clusters;
    vk::BufferPointer<GfxBatches> batches;
    vk::BufferPointer<GfxPositions> positions;
    vk::BufferPointer<GfxShadingHeap> shading;
    vk::BufferPointer<GfxIndices> indices;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    vk::BufferPointer<GfxMeshBatchList> batchList;
    uint cullFlags;
    uint width;
    uint height;
    uint listCount;
    uint pad0;
    uint pad1;
};
