// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define DOF_PASS
#include "common.hlsli"
#include "dof.hlsli"

struct PSIn
{
    float4 pos : SV_Position; // full-res pixel coords (depth .Load)
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/dof.h DofLowPush (56 bytes).
struct DofLowPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint pad0;
    float focusDistance;
    float focusRange;
    float maxRadiusPx;
    float2 texelSize;
    uint pad1;
    uint pad2;
    uint pad3;
};

[[vk::push_constant]]
ConstantBuffer<DofLowPush> dp;

// 12-tap Poisson disc (unit radius).
static const float2 kPoisson12[12] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = dp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(dp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(dp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(dp.samplerIndex)];

    int2 px = int2(IN.pos.xy);
    float dCenter = depthTex.Load(int3(px, 0)).r;

    // Pixel-center NDC (framebuffer y-down, matches ssao.frag.hlsl).
    float2 invExtent = dp.texelSize;
    float2 ndcXY = float2((float(px.x) + 0.5f) * invExtent.x * 2.0f - 1.0f,
                          (float(px.y) + 0.5f) * invExtent.y * 2.0f - 1.0f);
    float centerDist = DofViewDist(dCenter, ndcXY, frame.invProj);
    float coc = DofCoc01(centerDist, dp.focusDistance, dp.focusRange);
    float radiusPx = coc * max(dp.maxRadiusPx, 0.0f);

    // Sharp: keep the center tap (sky in focus included).
    if (radiusPx < 0.5f)
    {
        OUT.outColor = float4(SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb), 1.0f);
        return OUT;
    }

    bool centerBg = DofIsBackground(dCenter);
    float3 acc = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);
    float wSum = 1.0f;
    for (uint i = 0; i < 12; ++i)
    {
        float2 tapUv = IN.uv + kPoisson12[i] * radiusPx * dp.texelSize;
        float3 tap = SanitizeHdr(hdrTex.Sample(smp, tapUv).rgb);
        // Silhouette guard (the documented low-quality tradeoff, damped):
        // sky centers reject sharp-foreground taps and foreground centers
        // reject sky taps, so edges smear less. Not depth-weighted beyond
        // this — HQ bokeh is the upgrade path.
        int2 tapPx = int2(tapUv * float2(1.0f / max(invExtent.x, 1e-9f),
                                         1.0f / max(invExtent.y, 1e-9f)));
        float tapD = depthTex.Load(int3(tapPx, 0)).r;
        bool tapBg = DofIsBackground(tapD);
        float w = 1.0f;
        if (centerBg != tapBg)
            w = 0.15f;
        acc += tap * w;
        wSum += w;
    }
    OUT.outColor = float4(acc / max(wSum, 1e-5f), 1.0f);
    return OUT;
}
