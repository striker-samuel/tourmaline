// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include "vertex_pull.hlsli"

// Reusable packed chart-vertex decode for a future indexed VS and the
// existing mesh shader. This file intentionally declares NO global
// descriptor or active entry point and changes no shader permutation.
#define LM_PULL_NO_CHART 0xFFFFFFFFu
#define LM_PULL_NO_LOBES 0xFFFFFFFFu

struct LightmapPullVertex
{
    float2 chartUv;
    uint chartId;
};

LightmapPullVertex DecodeLightmapPullVertex(uint2 packedWords)
{
    LightmapPullVertex outVertex;
    outVertex.chartUv = float2(
        (float)(packedWords.x & 0xFFFFu),
        (float)((packedWords.x >> 16u) & 0xFFFFu)) * (1.0f / 65535.0f);
    outVertex.chartId = packedWords.y;
    return outVertex;
}

bool LightmapPullEnabled(uint lobeCount, bool streamResident)
{
    // Do not conflate a VALID additive delta of 0xFFFFFFFF (-1)
    // with an absent stream; residency is published explicitly.
    return lobeCount != LM_PULL_NO_LOBES && streamResident;
}

uint2 LightmapPullLoadWords(ByteAddressBuffer heap, uint globalVertexId,
    uint additiveDelta)
{
    const uint streamVertex =
        VertexPullMapUnchecked(globalVertexId, additiveDelta);
    return heap.Load2(VertexPullByteOffsetUnchecked(
        streamVertex, VP_LIGHTMAP_STRIDE_BYTES));
}
