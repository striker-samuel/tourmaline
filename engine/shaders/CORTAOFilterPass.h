// CORTAO Carbon Engine Implementation v1.0.0
//
// © 2026 Fenris Creations. All rights reserved.

#include "CORTAOBase.h"

#ifndef CORTAO_FILTER_BENT_NORMAL
#define CORTAO_FILTER_BENT_NORMAL 0
#endif

#ifndef CORTAO_LARGE_FILTER
#define CORTAO_LARGE_FILTER 0
#endif

#ifndef CORTAO_FILTER_SYMMETRY
#define CORTAO_FILTER_SYMMETRY 0
#endif

uint4 CortaoGatherPackedData(float2 texCoords, int2 offset);

#if CORTAO_FILTER_BENT_NORMAL
float4x4 CortaoGatherSSAO(float2 texCoords, int2 offset);
void CortaoWriteFilterOutput(int2 pixelCoords, float4 ssao);
#else
float4 CortaoGatherSSAO(float2 texCoords, int2 offset);
void CortaoWriteFilterOutput(int2 pixelCoords, float ssao);
#endif

struct CORTAOFilterPassParameters {
    uint packedResolution;
    float depthSensitivity;
    float normalSensitivity;
    uint _padding0;
};

struct CORTAOWeightParams {

    float4 depthMul;
    float depthOffset;
    float4 deltaX;
    float4 deltaY;

    int4 centerNormalX;
    int4 centerNormalY;

    float normalErrorMul;
};

int4 CortaoUnpackNormalX(uint4 packedData) {
    int normalBits = int(CORTAO_PACKED_NORMAL_BITS / 2);
    return (int4(packedData) << (32u - normalBits)) >> (32u - normalBits);
}

int4 CortaoUnpackNormalY(uint4 packedData) {
    int normalBits = int(CORTAO_PACKED_NORMAL_BITS / 2);
    return (int4(packedData) << (32u - normalBits * 2)) >> (32u - normalBits);
}

float4 calculateWeight(CORTAOWeightParams params, uint4 packedData, int2 offset) {
    float4 sampleDepth =
        float4(CortaoFma(CortaoUnpackDepth(packedData), params.depthMul, params.depthOffset.xxxx));
    float4 error = abs(CortaoFma(params.deltaX,
                                 float4(offset.xxxx),
                                 CortaoFma(params.deltaY, float4(offset.yyyy), sampleDepth)));

    int4 normalX = CortaoUnpackNormalX(packedData) - params.centerNormalX;
    int4 normalY = CortaoUnpackNormalY(packedData) - params.centerNormalY;
    float4 normalError = float4(normalX * normalX + normalY * normalY); // can't use FMA here...
    error = CortaoFma(normalError, params.normalErrorMul.xxxx, error);

    return exp2(-error);
}

#if CORTAO_FILTER_BENT_NORMAL

float4x4 CortaoFilterAccumulate(float4x4 ssaoSum, float2 texCoords, int2 offset, float4 weight) {
    float4x4 data = CortaoGatherSSAO(texCoords, offset);
    [unroll] for (uint i = 0u; i < 4u; i++) {
        ssaoSum[i] = CortaoFma(data[i], weight[i].xxxx, ssaoSum[i]);
    }
    return ssaoSum;
}
#else

float4 CortaoFilterAccumulate(float4 ssaoSum, float2 texCoords, int2 offset, float4 weight) {
    return CortaoFma(CortaoGatherSSAO(texCoords, offset), weight, ssaoSum);
}
#endif

void CortaoFilterPass(uint2 threadCoords, CORTAOFilterPassParameters parameters) {
    uint2 resolution = CortaoGetResolution(parameters.packedResolution);

    uint2 pixelCoords = (threadCoords << uint2(1, 1)) | uint2(1, 1);

    //">" is correct here, as we write to pixelCoords.x-1 as well.
    if (pixelCoords.x > resolution.x || pixelCoords.y > resolution.y) {
        return;
    }

    float2 texCoords = float2(pixelCoords) / float2(resolution);

    uint4 centerPackedData = CortaoGatherPackedData(texCoords, int2(0, 0));

    if (centerPackedData.x == 0u && centerPackedData.y == 0u && centerPackedData.z == 0u &&
        centerPackedData.w == 0u) {
        return;
    }

    float4 centerDepth = CortaoUnpackDepth(centerPackedData);
    float4 inverseCenterDepth = 1.0 / centerDepth;

    float4 deltaX = float4((centerDepth.xxww - centerDepth.yyzz) * inverseCenterDepth);
    float4 deltaY = float4((centerDepth.wzzw - centerDepth.xyyx) * inverseCenterDepth);

    CORTAOWeightParams weightParams;

    weightParams.depthMul = parameters.depthSensitivity * inverseCenterDepth;
    weightParams.depthOffset = -parameters.depthSensitivity;
    weightParams.deltaX = deltaX * float(parameters.depthSensitivity);
    weightParams.deltaY = deltaY * float(parameters.depthSensitivity);

    weightParams.centerNormalX = CortaoUnpackNormalX(centerPackedData);
    weightParams.centerNormalY = CortaoUnpackNormalY(centerPackedData);

    uint normalBits = uint(CORTAO_PACKED_NORMAL_BITS / 2);
    weightParams.normalErrorMul =
        float(parameters.normalSensitivity * (1.0 / float(1u << normalBits)));

#if CORTAO_FILTER_BENT_NORMAL
    float4x4 ssaoSum = float4x4(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
#else
    float4 ssaoSum = float4(0, 0, 0, 0);
#endif

    ssaoSum = CortaoFilterAccumulate(ssaoSum, texCoords, int2(0, 0), float4(1, 1, 1, 1));
    float4 weightSum = float4(1, 1, 1, 1);

#if CORTAO_LARGE_FILTER
    uint sampleCount = 6;
    int2 offsets[6];
    offsets[0] = int2(3, 0);
    offsets[1] = int2(6, 0);
    offsets[2] = int2(3, 3);
    offsets[3] = int2(0, 3);
    offsets[4] = int2(0, 6);
    offsets[5] = int2(3, -3);

#else
    uint sampleCount = 2;
    int2 offsets[2];
    offsets[0] = int2(3, 0);
    offsets[1] = int2(0, 3);
#endif

    [unroll] for (uint i = 0u; i < sampleCount; i++) {

        int2 offset = offsets[i];

        float4 weightA =
            calculateWeight(weightParams, CortaoGatherPackedData(texCoords, -offset), -offset);
        float4 weightB =
            calculateWeight(weightParams, CortaoGatherPackedData(texCoords, +offset), +offset);

#if CORTAO_FILTER_SYMMETRY
        float4 minWeight = min(weightA, weightB);
        weightA = minWeight;
        weightB = minWeight;
        weightSum = CortaoFma(minWeight, float(2.0).xxxx, weightSum);
#else
        weightSum += weightA + weightB;
#endif

        ssaoSum = CortaoFilterAccumulate(ssaoSum, texCoords, -offset, weightA);
        ssaoSum = CortaoFilterAccumulate(ssaoSum, texCoords, +offset, weightB);
    }

    float4 inverseWeight = float(1.0) / float4(weightSum);
    [unroll] for (uint i = 0u; i < 4u; i++) {
        ssaoSum[i] *= inverseWeight[i];
    }

    uint4 coords = uint4(pixelCoords - 1u, pixelCoords);
    uint2 outputCoordArray[4];
    outputCoordArray[0] = coords.xw;
    outputCoordArray[1] = coords.zw;
    outputCoordArray[2] = coords.zy;
    outputCoordArray[3] = coords.xy;
    [unroll] for (uint i = 0u; i < 4u; i++) {

        uint2 outputCoords = outputCoordArray[i];

        if (centerPackedData[i] != 0u && outputCoords.x < resolution.x &&
            outputCoords.y < resolution.y) {
            CortaoWriteFilterOutput(int2(outputCoords), ssaoSum[i]);
        }
    }
}