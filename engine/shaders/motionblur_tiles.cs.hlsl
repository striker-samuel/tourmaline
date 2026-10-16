// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Motion blur tile reduction, phase A (per-tile max velocity).
//
// One 16x16 group per screen tile: every thread Loads its pixel's motion
// vector (prevUV - currUV, RG16F), strips the temporal-jitter component
// and scales by the shutter strength — the exact transform the gather
// applies — and the group argmax-reduces to the MAX-LENGTH vector via
// groupshared. Thread 0 writes it to the raw tile map (RGBA32F storage,
// one texel per tile, xy = velocity in UV).
//
// Phase B (motionblur_tiledilate.cs.hlsl) max-filters this map over the
// 5x5 tile neighborhood; the gather (motionblur.frag.hlsl) then lets any
// pixel adopt the tile velocity when it beats its own, so revealed
// background streaks with the foreground that vacated it instead of
// staying sharp around silhouettes (CoD:AW-style tile dilation).
//
// Tile size MUST match kMotionBlurTilePixels (16) in
// Engine/Rendergraph/motionblur.h and [numthreads] here.
//
// C++ mirror: smvk::MotionBlurTilesPush in motionblur.h (32 bytes).

#define MOTIONBLUR_TILES_PASS
#include "common.hlsli"

// Bindless storage image array (set 0, binding 2, GENERAL layout —
// mirrors binding 0 for Storage-usage images, see
// VulkanResourceManager::UpdateDescriptors). float4 matches the RGBA32F
// tile map: a format-mismatched OpTypeImage operand is undefined for the
// whole image (same rule as evsm_blur).
[[vk::binding(2, 0)]]
RWTexture2D<float4> motionBlurStorage[];

// Must match Engine/Rendergraph/motionblur.h MotionBlurTilesPush (32 bytes).
struct MotionBlurTilesPush
{
    uint motionTextureIndex;   // bindless RG16F motion vectors
    uint tileMaxTextureIndex;  // storage slot of the raw tile map
    uint srcWidth;             // source extent (partial-edge guard)
    uint srcHeight;
    float2 jitterDelta;        // (prevJitter - currJitter) UV, stripped
    float velocityScale;       // shutter strength
    float pad0;
};

[[vk::push_constant]]
ConstantBuffer<MotionBlurTilesPush> pcTiles;

groupshared float gsLen2[256];
groupshared float2 gsVel[256];

[numthreads(16, 16, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint linearTid = threadId.y * 16u + threadId.x;

    uint2 px = groupId.xy * 16u + threadId.xy;
    float2 vel = float2(0.0f, 0.0f);
    if (px.x < pcTiles.srcWidth && px.y < pcTiles.srcHeight)
    {
        // TRUE velocity (jitter stripped, shutter scaled): the gather
        // compares own vs tile velocity in this exact domain.
        vel = (textures[NonUniformResourceIndex(pcTiles.motionTextureIndex)]
            .Load(int3((int)px.x, (int)px.y, 0)).rg - pcTiles.jitterDelta)
            * pcTiles.velocityScale;
        if (!isfinite(vel.x + vel.y))
            vel = float2(0.0f, 0.0f);
    }

    gsLen2[linearTid] = dot(vel, vel);
    gsVel[linearTid] = vel;
    GroupMemoryBarrierWithGroupSync();

    // Argmax reduce (Hillis-Steele over 256 lanes): keeps the VECTOR of
    // the longest velocity, not just its length. Ties keep the lower
    // lane's vector — irrelevant, equal lengths blur alike.
    [unroll]
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (linearTid < stride)
        {
            if (gsLen2[linearTid + stride] > gsLen2[linearTid])
            {
                gsLen2[linearTid] = gsLen2[linearTid + stride];
                gsVel[linearTid] = gsVel[linearTid + stride];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (linearTid == 0u)
    {
        motionBlurStorage[NonUniformResourceIndex(pcTiles.tileMaxTextureIndex)]
            [groupId.xy] = float4(gsVel[0], 0.0f, 0.0f);
    }
}
