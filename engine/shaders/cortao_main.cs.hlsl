// SPDX-License-Identifier: MIT
// CORTAO main pass (Carbon Engine, MIT): packed data + occluder LUT ->
// occlusion (R32_FLOAT). SM 6.7 workaround: packed data is R32_FLOAT
// (asuint(asfloat)), the occluder LUT is R16_UNORM (scale back to uint).

#define CORTAO_PACKED_NORMAL_BITS 10
#define CORTAO_SAMPLE_COUNT 16
#define CORTAO_RAY_COUNT 16
#define CORTAO_OCCLUDER_LOOKUP_TABLE 1
#define CONTACT_SHADOW_PASS // standalone compute push (no scene pc)
#include "CORTAOMainPass.h"
#include "common.hlsli"
#include "ao.hlsli"

[[vk::binding(2, 0)]]
RWTexture2D<float> cortaoStorage[];

struct CortaoMainPassPush {
    uint packedTextureIndex;    // mipmapped R32_FLOAT (bindless sampled)
    uint occluderLutIndex;      // 3D R16_UNORM (bindless volume)
    uint normalTextureIndex;    // full-res G-buffer oct normal
    uint occlusionIndex;        // storage bindless index of the R32 output
    uint nearestBorderSampler;  // point + border (packed data)
    uint nearestClampSampler;   // point + clamp (occluder LUT)
    uint pad0;
    uint pad1;
    // CORTAOMainPassParameters:
    uint packedResolution;
    uint randomSeed;
    float normalBias;
    float maxApparentCircleRadiusCoefficient;
    float4 unprojectParams;
    float4 projectionParams;
    float mipBias;
    float inverseMaxSlopeCompensation;
    float backfaceThreshold;
    float backfaceBoost;
    GPU_PTR_READ(FrameUniforms) frame;
};
GPU_PUSH(CortaoMainPassPush, sp);

uint CortaoSamplePackedData(float2 texCoords, float mipmap) {
    const float v = textures[NonUniformResourceIndex(sp.packedTextureIndex)]
                        .SampleLevel(samplers[NonUniformResourceIndex(sp.nearestBorderSampler)],
                                     texCoords, mipmap).r;
    return asuint(v);
}

float3 CortaoLoadNormal(int2 px) {
    const float2 oct =
        textures[NonUniformResourceIndex(sp.normalTextureIndex)].Load(int3(px, 0)).xy;
    const float3 worldN = AoOctDecode(oct);
    return mul((float3x3)sp.frame.Get().view, worldN);
}

uint CortaoSampleOccluderLookupTable(float3 texCoords) {
    const float v = volumeTextures[NonUniformResourceIndex(sp.occluderLutIndex)]
                        .SampleLevel(samplers[NonUniformResourceIndex(sp.nearestClampSampler)],
                                     texCoords, 0.0).r;
    return uint(v * 65535.0 + 0.5);
}

void CortaoWriteResult(int2 px, float occlusion, float3 bentNormal,
                       float3x3 tangentSpaceTransform, uint rayBits) {
    cortaoStorage[NonUniformResourceIndex(sp.occlusionIndex)][px] = occlusion;
}

uint2 Deinterleave(uint index) {
    // Morton deinterleave of SV_GroupIndex (0..255) into a 16x16 tile.
    uint x = index & 0x55u;
    uint y = (index >> 1u) & 0x55u;
    x = (x | (x >> 1u)) & 0x33u;
    x = (x | (x >> 2u)) & 0x0fu;
    y = (y | (y >> 1u)) & 0x33u;
    y = (y | (y >> 2u)) & 0x0fu;
    return uint2(x, y);
}

#define WORKGROUP_SIZE 16
[numthreads(WORKGROUP_SIZE, WORKGROUP_SIZE, 1)]
void mainCS(uint2 workgroupID : SV_GroupID, uint localIndex : SV_GroupIndex) {
    const uint2 px = workgroupID * uint2(WORKGROUP_SIZE, WORKGROUP_SIZE) + Deinterleave(localIndex);
    CORTAOMainPassParameters params;
    params.packedResolution = sp.packedResolution;
    params.randomSeed = sp.randomSeed;
    params.normalBias = sp.normalBias;
    params.maxApparentCircleRadiusCoefficient = sp.maxApparentCircleRadiusCoefficient;
    params.unprojectParams = sp.unprojectParams;
    params.projectionParams = sp.projectionParams;
    params.mipBias = sp.mipBias;
    params.inverseMaxSlopeCompensation = sp.inverseMaxSlopeCompensation;
    params.backfaceThreshold = sp.backfaceThreshold;
    params.backfaceBoost = sp.backfaceBoost;
    CortaoMainPass(px, params);
}
