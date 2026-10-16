// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Terrain push block: DXC allows a SINGLE push_constant block per entry, so
// terrain shaders that need the per-draw tail declare ONE block here.
// Bytes 0..192 mirror the scene PushConstants EXACTLY (same field order as
// push_constants.hlsli — the app pushes the scene pc first, then the 32B
// tail at 192); bytes 192..224 carry the C++ TerrainBlockPush tail
// (Rendergraph/terrain.h, static_assert(sizeof==32) pins it).
// Shaders using this block MUST #define TERRAIN_NOPC before including
// common.hlsli (excludes the scene pc from that stage — same split as
// DEFAULT_DIRECT_PASS / PLACEMENT_INSTANCED_PASS).
// Drift rule: any field added to PushConstants below byte 192 must be
// mirrored here (like DefaultVsPush in default.vert.hlsl).

struct TerrainPushBlock
{
    float4x4 model;
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<LightBuffer> bLights;
    vk::BufferPointer<MaterialsBuffer> bMats;
    vk::BufferPointer<DirectionalCascadeBuffer> cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights;
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    vk::BufferPointer<SpotShadowBuffer> spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    uint pad_pc;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint mainRenderView;
    uint renderViewIndex;
    // Terrain tail (C++ TerrainBlockPush words).
    uint atlasSlot;
    float originX;
    float originZ;
    float invWorldSize;
    float baseY;
    float range;
    int hfShadow;
    int hfSteps;
};

[[vk::push_constant]]
ConstantBuffer<TerrainPushBlock> pcT;
