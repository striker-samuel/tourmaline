// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// GPU Scene task + mesh stage interface. Experimental, not yet submitted by
// WorldRenderer. The producer must prove topology and stream residency first.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"
#include "hzb.hlsli"

#define GS_TASK_THREADS 32u
#define GS_MESH_THREADS 32u
#define GS_MESH_MAX_TRIS 96u
#define GS_MESH_MAX_VERTS 64u
#define GS_MAX_TASK_GROUPS 2097152u
#define GS_MAX_WORK 1048576u
#define GS_MAX_INSTANCES 4194304u
#define GS_MAX_CLUSTERS 4194304u
#define GS_MAX_GEO 65535u
#define GS_MAX_VERTICES 44739242u
#define GS_MAX_INDEX_WORDS (65536u * 128u * 3u)

struct GsTaskGroup {
    uint workSlot;
    uint firstCluster;
    uint clusterCount;
    uint rasterClass;
};
struct GsTaskGroups { GsTaskGroup rows[GS_MAX_TASK_GROUPS]; };
struct GsWorkRow { uint4 identity; uint4 detail; };
struct GsWork { GsWorkRow rows[GS_MAX_WORK]; };
struct GsCluster { uint4 a; float4 b; float4 c; float4 d; uint4 e; };
struct GsClusters { GsCluster rows[GS_MAX_CLUSTERS]; };
struct GsInstances { GpuSceneInstanceRow rows[GS_MAX_INSTANCES]; };
struct GsView {
    DoubleFloat3 origin;
    float4 planes[6];
};
struct GsRasterView {
    float4x4 relativeViewProjection;
    DoubleFloat3 origin;
};
struct GsIndexedGeometry { uint indexCount; uint firstIndex; int vertexOffset; uint flags; };
struct GsGeometry { GsIndexedGeometry rows[GS_MAX_GEO]; };
struct GsAliases { uint slots[GS_MAX_GEO]; };
struct GsIndices { uint index[GS_MAX_INDEX_WORDS]; };
struct GsPositions { uint word[GS_MAX_VERTICES * 3u]; };
struct GsShading { uint word[GS_MAX_VERTICES * 4u]; };
// Indexed-meshlet streams (mesh shader): each cluster's unique local vertex
// window (global vertex ids, lifted) and its local triangle ids (3 packed u8
// ids per uint, one uint per triangle). Sized by the same vertex/index caps
// as the position/indices heaps (the unique window is a subset of each).
struct GsMeshletVerts { uint id[GS_MAX_VERTICES]; };
struct GsMeshletTris { uint tri[GS_MAX_INDEX_WORDS / 3u]; };

// Exact meshlet history key: persistent instance slot + generation + global cluster id.
// The hash table is a cache, not an authority for topology or residency.
#define GS_TEMPORAL_HASH_LIMIT 1048576u
struct GsTemporalEntry { uint state; uint slot; uint generation; uint clusterId; };
struct GsTemporalHistory {
    uint overflow;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    GsTemporalEntry entries[GS_TEMPORAL_HASH_LIMIT];
};

// This struct is a GPU-addressable parameter buffer, NOT push constants.
// Both shader stages receive a short push containing its BDA, group slice
// and raster class. Do not change its member order without an ABI check.
struct GsTaskOffsets { uint values[GS_MAX_WORK + 1u]; };
struct GsTaskMeshParams {
    GPU_PTR_READ(GsTaskGroups) groups;
    GPU_PTR_READ(GsWork) work;
    GPU_PTR_READ(GsClusters) clusters;
    GPU_PTR_READ(GsInstances) instances;
    GPU_PTR_READ(GsView) cullView;
    GPU_PTR_READ(GsRasterView) rasterView;
    GPU_PTR_READ(GsGeometry) geometry;
    GPU_PTR_READ(GsAliases) aliases;
    GPU_PTR_READ(GsIndices) indices;
    GPU_PTR_READ(GsPositions) positions;
    GPU_PTR_READ(GsShading) shading;
    GPU_PTR_READ(GsTaskOffsets) taskOffsets;
    uint workCount;
    uint instanceCount;
    uint clusterCount;
    uint geometryCount;
    uint aliasCount;
    uint indexBase;
    uint indexWordCapacity;
    uint vertexCapacity;
    uint viewValid;
    uint coneEnabled;
    uint workCapacity;
    uint debugMode; // 0 = lit, 1 = meshlet colors, 2 = LOD colors
    GPU_PTR_READ(HzbBuffer) hiz; // reversed-Z MIN Hi-Z (meshlet occlusion), null = disabled
    uint hizWidth;
    uint hizHeight;
    uint hizMips;
    uint hizValid; // 1 = run the meshlet occlusion test
    GPU_PTR_READ(GsMeshletVerts) meshletVertices; // indexed-meshlet unique vertex windows
    GPU_PTR_READ(GsMeshletTris) meshletTriangles; // indexed-meshlet local triangle ids (3 u8/uint)
    GPU_PTR_READ(GsTemporalHistory) historyPrevious;
    GPU_PTR(GsTemporalHistory) historyCurrent;
    uint historyCapacity;
    uint historyValid;
};
struct GsTaskMeshPush {
    // Preserve the fragment shader's 192-byte scene push constants.
    float4x4 _scenePad0;
    float4x4 _scenePad1;
    float4x4 _scenePad2;
    GPU_PTR_READ(GsTaskMeshParams) params;
    uint groupBase;
    uint groupCount;
    uint rasterClass;
    uint taskGridX; // x dimension of indirect task dispatch (for 2D flattening)
    uint temporalPhase; // 0 normal, 1 seed, 2 remainder, 3 Main
    uint reservedTemporal;
};

// Workgroup-local payload. Each mesh workgroup reads its own compact slot.
struct GsTaskPayload {
    uint workSlot;
    uint surviving;
    uint clusterIds[GS_TASK_THREADS];
};
