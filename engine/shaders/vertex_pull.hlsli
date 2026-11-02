// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Opt-in vertex-pulling utilities. Nothing includes this from an active
// raster shader yet. No descriptor binding or pipeline ABI is introduced.
// The caller supplies a ByteAddressBuffer (when one is bound in a later PR)
// and an address in that buffer's own vertex-index space.
#define VP_POSITION_STRIDE_BYTES 12u
#define VP_SHADING_STRIDE_BYTES 16u
#define VP_LIGHTMAP_STRIDE_BYTES 8u

// Unsigned wraparound is intentional: this is also the draw-ring delta
// arithmetic for independently packed lightmap streams. Presence must NOT
// be inferred from whether the delta equals 0xFFFFFFFFu.
uint VertexPullMapUnchecked(uint globalVertexId, uint additiveDelta)
{
    return globalVertexId + additiveDelta;
}

uint VertexPullByteOffsetUnchecked(uint streamVertex, uint strideBytes)
{
    return streamVertex * strideBytes;
}

// The position heap packs float3 at 12-byte stride (not 16-byte-aligned
// float3 array elements). Use word loads, exactly as the mesh shader does.
float3 VertexPullLoadPosition(ByteAddressBuffer positions, uint vertexId)
{
    uint3 xyz = positions.Load3(
        VertexPullByteOffsetUnchecked(vertexId, VP_POSITION_STRIDE_BYTES));
    return asfloat(xyz);
}

// Returns the four PACKED shading words. Existing vertex_pack.hlsli
// performs color/half-UV/oct normal/tangent decoding after the load.
uint4 VertexPullLoadShadingWords(ByteAddressBuffer shading, uint vertexId)
{
    return shading.Load4(
        VertexPullByteOffsetUnchecked(vertexId, VP_SHADING_STRIDE_BYTES));
}
