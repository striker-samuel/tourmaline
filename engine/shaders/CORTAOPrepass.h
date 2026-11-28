// CORTAO Carbon Engine Implementation v1.0.0
//
// © 2026 Fenris Creations. All rights reserved.

#include "CORTAOBase.h"

float CortaoLoadDepth(int2 pixelCoords);
float3 CortaoLoadNormal(int2 pixelCoords);

void CortaoWritePackedData(int2 pixelCoords, int mipLevel, uint data);

struct CORTAOPrepassParameters {
    uint packedResolution;
    float depthScale;
    float depthOffset;
    uint randomSeed;

    uint mipCount;
    uint _padding0;
    uint _padding1;
    uint _padding2;
};

void CortaoPrepass(uint2 pixelCoords, CORTAOPrepassParameters params) {

    // check if our pixel is out-of-bounds and early out in that case
    uint2 resolution = CortaoGetResolution(params.packedResolution);
    if (pixelCoords.x >= resolution.x || pixelCoords.y >= resolution.y) {
        return;
    }

    float depth = CortaoLoadDepth(int2(pixelCoords));

    uint packedData = 0u;

    float skyBoxDepth = float(params.depthScale < 0.0); // Handles both standard and inverted depth
    if (depth != skyBoxDepth) {

        // Calculate the linear depth
        float linearDepth = 1.0 / CortaoFma(depth, params.depthScale, params.depthOffset);

        float3 normal = CortaoLoadNormal(int2(pixelCoords));

        // Pack a spheremap encoded normal
        uint seed = CortaoHash(CortaoSeed(pixelCoords) + params.randomSeed);
        float2 dither = float2(uint2(seed & 0xFFFFu, seed >> 16u)) * (1.0 / float(0xFFFFu));
        uint mask = (1 << CORTAO_PACKED_NORMAL_BITS) - 1;
        uint packedNormal = CortaoEncodeNormal(normal, dither);

        // Merge the linear depth and the packed normal
        packedData = (asuint(linearDepth) & ~mask) | (packedNormal & mask);
    }

    CortaoWritePackedData(int2(pixelCoords), 0, packedData);

    [unroll] for (int mipLevel = 1; mipLevel < 8; mipLevel++) {

        uint2 nextCoords = pixelCoords >> 1u;

        uint2 dither =
            uint2(params.randomSeed, params.randomSeed) >> uint2(mipLevel * 2, mipLevel * 2 + 1);

        bool wrongPixel = bool(((pixelCoords.x ^ nextCoords.y ^ dither.x) |
                                (pixelCoords.y ^ nextCoords.x ^ dither.y)) &
                               0x1u);

        pixelCoords = nextCoords;
        resolution = max(resolution >> 1u, 1u);

        if (any(bool4(mipLevel >= params.mipCount,
                      wrongPixel,
                      pixelCoords.x >= resolution.x,
                      pixelCoords.y >= resolution.y))) {
            return;
        }

        CortaoWritePackedData(int2(pixelCoords), mipLevel, packedData);
    }
}