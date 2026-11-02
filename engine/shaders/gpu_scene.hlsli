// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Mirrored layout for Engine/Renderer/gpu_scene.h; currently declarations
// only. The subsequent GPU visibility PR will define buffer-pointer readers,
// generation validation and persistent -> transient expansion.
#include "double_float.hlsli"

struct GpuSceneMesh {
    uint firstLod;
    uint lodCount;
    uint residentMask;
    uint flags;
};

// Lod flags: 1 = resident payload, 2 = bound metadata known,
// 4 = registered in the transient meshlet geometry table.
// Mesh flags: 1 = conservative bounds known for ALL LODs.
struct GpuSceneLod {
    uint firstSubmesh;
    uint submeshCount;
    float simplifyError;
    uint flags;
};

struct GpuSceneSubmesh {
    uint geometryId;
    uint materialId;
    uint pipelineGroup;
    uint passMask;
};

struct GpuSceneInstance {
    DoubleFloat3 worldPosition;
    float4 basisX;
    float4 basisY;
    float4 basisZ;
    float4 localSphere;
    uint4 refs;
};

// Instance references are never draw IDs: generation must match the
// GpuSceneInstanceRow identity before GPU culling/draw expansion.
struct GpuSceneLeafMember { uint4 ref; };
// GpuSceneLeaf.range: x = first LeafMember row, y = member count,
// z bit 0 = all member bounds complete, w reserved.
struct GpuSceneLeaf {
    DoubleFloat3 worldCenter;
    float4 halfExtents;
    uint4 range;
};

// GPU row identity: x generation, y alive. ALWAYS validate both when
// a future GPU consumer follows a generational reference.
struct GpuSceneMeshRow { GpuSceneMesh value; uint4 identity; };
struct GpuSceneLodRow { GpuSceneLod value; uint4 identity; };
struct GpuSceneSubmeshRow { GpuSceneSubmesh value; uint4 identity; };
struct GpuSceneInstanceRow { GpuSceneInstance value; uint4 identity; };
struct GpuSceneLeafRow { GpuSceneLeaf value; uint4 identity; };
struct GpuSceneLeafMemberRow { GpuSceneLeafMember value; uint4 identity; };
