// SPDX-License-Identifier: MIT
// CORTAO filter pass (Carbon Engine, MIT): sparse bilateral blur of the
// occlusion output. Occlusion-only (no bent normal). SM 6.7 workaround:
// the packed data is R32_FLOAT, gathered and converted back to uint.

#define CORTAO_PACKED_NORMAL_BITS 10
#define CORTAO_FILTER_BENT_NORMAL 0
#define CONTACT_SHADOW_PASS // standalone compute push (no scene pc)
#include "CORTAOFilterPass.h"
#include "common.hlsli"

[[vk::binding(2, 0)]]
RWTexture2D<float> cortaoStorage[];

struct CortaoFilterPassPush {
    uint packedTextureIndex;    // mipmapped R32_FLOAT (bindless sampled)
    uint occlusionInIndex;      // main-pass occlusion (R32, bindless sampled)
    uint occlusionOutIndex;     // filtered occlusion (storage bindless index)
    uint nearestBorderSampler;  // point + border (packed data gather)
    // CORTAOFilterPassParameters:
    uint packedResolution;
    float depthSensitivity;
    float normalSensitivity;
    uint pad0;
};
GPU_PUSH(CortaoFilterPassPush, sp);

uint4 CortaoGatherPackedData(float2 texCoords, int2 offset) {
    const float4 v = textures[NonUniformResourceIndex(sp.packedTextureIndex)]
                         .Gather(samplers[NonUniformResourceIndex(sp.nearestBorderSampler)],
                                 texCoords, offset);
    return asuint(v);
}

float4 CortaoGatherSSAO(float2 texCoords, int2 offset) {
    return textures[NonUniformResourceIndex(sp.occlusionInIndex)]
        .Gather(samplers[NonUniformResourceIndex(sp.nearestBorderSampler)], texCoords, offset);
}

void CortaoWriteFilterOutput(int2 px, float ssao) {
    cortaoStorage[NonUniformResourceIndex(sp.occlusionOutIndex)][px] = ssao;
}

[numthreads(8, 8, 1)]
void mainCS(uint2 threadCoords : SV_DispatchThreadID) {
    CORTAOFilterPassParameters params;
    params.packedResolution = sp.packedResolution;
    params.depthSensitivity = sp.depthSensitivity;
    params.normalSensitivity = sp.normalSensitivity;
    CortaoFilterPass(threadCoords, params);
}
