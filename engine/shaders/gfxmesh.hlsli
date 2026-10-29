// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
// a batch is mesh-drawn iff draw.drawSet == OPAQUE &&
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
// Shading heap as uint words (4 per vertex: color, uv, normal, tangent —
// see PackedVertexShading in Engine/Renderer/mesh.h). The lightmap words
// (chartUv, chartId) moved OUT to the OPTIONAL PackedVertexLightmap
// stream (2 words per vertex, its own heap): unlightmapped meshes don't
// carry it at all, so the fetch is guarded by the per-draw delta
// (GfxDraw.misc2.w = lightmapBase - meshVertexBase, 0xFFFFFFFF = none).
struct GfxShadingHeap { uint words[GFX_MAX_VERTICES * 4]; };
struct GfxLightmapHeap { uint words[GFX_MAX_VERTICES * 2]; };
// Compacted eligible batch indices for one variant dispatch.
struct GfxMeshBatchList { uint batch[GFX_MAX_BATCHES * 2]; };
// DEFORMED vertex heap: 5 words per vertex [pos3 asuint, packed normal,
// packed tangent], fetched at (vi - params.deformedBase) * 5 by skinned
// draws (GFX_DRAW_SKINNED) and by the cull chain's GfxSkinBounds pass.
// Declared in gfxfilter.hlsli (this header includes it) so the cull-chain
// push mirror can reference it without a circular include.

// Per-frame mesh params (one small device buffer, BDA in the 224 tail,
// uploaded ONCE per RecordMeshMain — never per span: per-span uploads to
// the same buffer race (transfer-queue writes vs graphics reads across
// dispatches in one pass). Per-dispatch slicing rides the tail push
// (listBase/listCount below), which is per-dispatch by construction.
// The CPU partitions eligible batches per FS variant (base/sg/gi, exactly
// the indexed leg's program selection), so the shader trusts its list —
// no eligibility predicate here.
//
// TWO matrices (the culling-freeze contract): viewProj is the CULL matrix
// (the gfx view record — the FROZEN one while the debug freeze holds the
// filter), outViewProj is the LIVE main-view matrix the rasterizer
// projects through (the indexed VS resolves the same through the live
// render-view slab; the two are equal when not frozen). CullTriangle runs
// against viewProj; SV_Position comes from outViewProj.
struct GfxMeshParamsData
{
    // Matrices FIRST: 16-aligned at offsets 0/64 on both sides (a pointer
    // run before them would leave offset 56, padded differently by HLSL
    // vs C++ packing — the static_assert below guards the whole blob).
    float4x4 viewProj;
    float4x4 outViewProj;
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<GfxClusters> clusters;
    vk::BufferPointer<GfxBatches> batches;
    vk::BufferPointer<GfxPositions> positions;
    vk::BufferPointer<GfxShadingHeap> shading;
    // The OPTIONAL lightmap stream (PackedVertexLightmap, 2 words/vertex,
    // INDEPENDENTLY packed windows): fetched as
    //   lm words[(vi + draw.misc2.w) * 2]
    // where misc2.w carries the per-draw additive delta (lightmapBase -
    // meshVertexBase; 0xFFFFFFFF = the draw's mesh carries no lightmap).
    vk::BufferPointer<GfxLightmapHeap> lightmap;
    vk::BufferPointer<GfxIndices> indices;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    vk::BufferPointer<GfxMeshBatchList> batchList;
    // This flight's indirect command array (written by the filter CS this
    // frame, before Main): cmds[batch].indexCount == 0 marks a cluster the
    // filter fully dropped (per-triangle cull OR the T4 whole-cluster
    // HZB-occlusion/contribution tests the mesh path deliberately does not
    // duplicate). Gating on it keeps mesh/indexed picture parity EXACT —
    // including under the debug culling freeze, where the held HZB drops
    // clusters the live view sees.
    vk::BufferPointer<GfxIndirect> indirect;
    uint cullFlags;
    uint width;
    uint height;
    uint listCount;
    // The per-frame DEFORMED buffer (the Northlight compute-skinning
    // output; C++ mirror GfxMeshParams - gfx_filter.h): skinned draws
    // (GFX_DRAW_SKINNED) fetch pos/normal/tangent words from it (indexed
    // vi - deformedBase), everything else from the bind-pose heaps.
    // 0 = static fetches (no skinned cluster this frame).
    vk::BufferPointer<GfxSkinDeformedHeap> deformed;
    uint deformedBase; // 0xFFFFFFFF = no skinned cluster
    uint pad0;
};
