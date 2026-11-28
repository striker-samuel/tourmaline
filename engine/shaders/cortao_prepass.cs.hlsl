// SPDX-License-Identifier: MIT
// CORTAO prepass (Carbon Engine, MIT): depth + normal -> packed mipmapped
// buffer. SM 6.7 workaround: the packed data lives in R32_FLOAT (float bits
// of the packed uint), so it samples through the engine's float bindless set.

#define CORTAO_PACKED_NORMAL_BITS 10
#define CONTACT_SHADOW_PASS // standalone compute push (no scene pc)
#include "CORTAOPrepass.h"
#include "common.hlsli"
#include "ao.hlsli"

[[vk::binding(2, 0)]]
RWTexture2D<float> cortaoStorage[];

struct CortaoPrepassPush {
    uint depthTextureIndex;   // full-res reversed-Z D32 (bindless sampled)
    uint normalTextureIndex;  // full-res G-buffer oct normal (bindless sampled)
    uint packedMip0Index;     // storage bindless index of mip 0 (mips 1..7 follow)
    uint pad0;
    uint packedResolution;    // width | height<<16
    float depthScale;
    float depthOffset;
    uint randomSeed;
    uint mipCount;
    uint pad1;
    uint pad2;
    uint pad3;
    GPU_PTR_READ(FrameUniforms) frame;
};
GPU_PUSH(CortaoPrepassPush, sp);

float CortaoLoadDepth(int2 px) {
    return textures[NonUniformResourceIndex(sp.depthTextureIndex)].Load(int3(px, 0)).r;
}

float3 CortaoLoadNormal(int2 px) {
    const float2 oct =
        textures[NonUniformResourceIndex(sp.normalTextureIndex)].Load(int3(px, 0)).xy;
    const float3 worldN = AoOctDecode(oct);
    return mul((float3x3)sp.frame.Get().view, worldN);
}

void CortaoWritePackedData(int2 px, int mip, uint data) {
    cortaoStorage[NonUniformResourceIndex(sp.packedMip0Index + uint(mip))][px] = asfloat(data);
}

[numthreads(8, 8, 1)]
void mainCS(uint2 px : SV_DispatchThreadID) {
    CORTAOPrepassParameters params;
    params.packedResolution = sp.packedResolution;
    params.depthScale = sp.depthScale;
    params.depthOffset = sp.depthOffset;
    params.randomSeed = sp.randomSeed;
    params.mipCount = sp.mipCount;
    CortaoPrepass(px, params);
}
